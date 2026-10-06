# -*- coding: utf-8 -*-
"""
BOSS 特效材质一键搭建（在 UE 编辑器里跑）

它做三件事：
  1) 把 Tools/VFXSource/*.png 导入成 /Game/Boss/VFX/Textures/ 下的纹理
  2) 建两个基础材质：
       M_BossSpriteAdd    —— 加法混合 / 无光照，用于「发光的东西」（弹体核心、拖尾、爆闪、冲击环）
       M_BossSpriteAlpha  —— 半透明 / 无光照，用于「要看得清形状的贴地圈」（预警圈）
  3) 建好材质实例：MI_BossGlow / MI_BossImpact / MI_BossRing / MI_BossTelegraph
     —— C++ 就是按这些路径加载的（见 DemoProjectile / DemoImpactBurst / DemoTrailComponent）

⚠ 为什么拖尾的材质必须用 PerInstanceCustomData3Vector：
  整条尾巴是一个 InstancedStaticMesh 的 N 个实例，头亮尾淡全靠「每实例颜色」。
  材质里没有这个节点的话，所有残影会一样亮 —— 不崩，但看起来像一串等亮的珠子。

运行方式（编辑器内）：
  菜单 Tools → Execute Python Script...，或 Output Log 的 Python 模式：
      exec(open(r"K:\\selfgame\\demo\\Tools\\SetupBossVFX.py", encoding="utf-8").read())

前置：Edit → Plugins 里启用「Python Editor Script Plugin」。
"""

import os
import sys

try:
    import unreal
except ImportError:
    print("[X] 这个脚本必须在 UE 编辑器的 Python 环境里跑（不是系统 python）")
    sys.exit(1)

VFX_DIR = "/Game/Boss/VFX"
TEX_DIR = VFX_DIR + "/Textures"

# unreal.Paths.project_dir() 给的是相对引擎二进制的路径（形如 ../../../selfgame/demo/），
# 必须转成绝对路径才能拿来做 os.path.join
PROJECT_ROOT = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
SOURCE_DIR = os.path.join(PROJECT_ROOT.rstrip("/\\"), "Tools", "VFXSource")

MEL = unreal.MaterialEditingLibrary
EAL = unreal.EditorAssetLibrary
AT = unreal.AssetToolsHelpers.get_asset_tools()


# ------------------------------------------------------------------ 小工具
def log(msg):
    unreal.log("[VFX] " + msg)


def warn(msg):
    unreal.log_warning("[VFX] " + msg)


def recreate_asset(name, package_path, cls, factory):
    """脚本要能重复跑：已存在就先删掉重建，否则会残留上次的节点图。"""
    full = "{}/{}".format(package_path, name)
    if EAL.does_asset_exist(full):
        EAL.delete_asset(full)
    return AT.create_asset(name, package_path, cls, factory)


def connect_out(src, dst, dst_input="A"):
    """把 src 的「主输出」接到 dst 的某个输入。

    不同表达式的主输出名不一样（TextureSample 是 RGB，其余多为空串），
    所以这里按实际输出名去试，避免写死一个名字结果连不上、材质变成全黑。
    """
    candidates = []
    try:
        candidates.extend(list(MEL.get_material_expression_output_names(src)))
    except Exception:
        pass
    candidates.extend(["RGB", ""])

    tried = set()
    for out_name in candidates:
        if out_name in tried:
            continue
        tried.add(out_name)
        try:
            if MEL.connect_material_expressions(src, out_name, dst, dst_input):
                return True
        except Exception:
            continue
    warn("连接失败：%s → %s.%s" % (src, dst, dst_input))
    return False


def connect_named(src, out_name, dst, dst_input):
    try:
        return MEL.connect_material_expressions(src, out_name, dst, dst_input)
    except Exception as e:
        warn("连接 %s.%s 失败：%s" % (src, out_name, e))
        return False


def vec_param(mat, name, color, x, y):
    node = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, x, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", color)
    return node


def scalar_param(mat, name, value, x, y):
    node = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, x, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", value)
    return node


def tex_param(mat, name, texture, x, y):
    node = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D, x, y)
    node.set_editor_property("parameter_name", name)
    if texture:
        node.set_editor_property("texture", texture)
    try:
        node.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    except Exception:
        pass
    return node


def mul(mat, a, b, x, y):
    node = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, x, y)
    connect_out(a, node, "A")
    connect_out(b, node, "B")
    return node


# ------------------------------------------------------------------ 1. 导入贴图
def import_textures():
    if not os.path.isdir(SOURCE_DIR):
        warn("找不到贴图源目录：%s" % SOURCE_DIR)
        warn("先跑 Tools/GenerateBossVFXTextures.py 生成 PNG")
        return {}

    pngs = sorted(f for f in os.listdir(SOURCE_DIR) if f.lower().endswith(".png"))
    if not pngs:
        warn("%s 里没有 png" % SOURCE_DIR)
        return {}

    if not EAL.does_directory_exist(TEX_DIR):
        EAL.make_directory(TEX_DIR)

    textures = {}
    for filename in pngs:
        name = os.path.splitext(filename)[0]
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", os.path.join(SOURCE_DIR, filename))
        task.set_editor_property("destination_path", TEX_DIR)
        task.set_editor_property("destination_name", name)
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", True)
        task.set_editor_property("factory", unreal.TextureFactory())

        AT.import_asset_tasks([task])

        tex = EAL.load_asset("{}/{}".format(TEX_DIR, name))
        if not tex:
            warn("导入失败：%s" % filename)
            continue

        # 这些贴图是「亮度遮罩」而不是色彩图：必须关掉 sRGB，
        # 否则中间调会被伽马推亮，发光边缘会显得脏、发灰。
        for prop, value in (
            ("compression_settings", unreal.TextureCompressionSettings.TC_DEFAULT),
            ("srgb", False),
            ("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_SIMPLE_AVERAGE),
            ("address_x", unreal.TextureAddress.TA_CLAMP),
            ("address_y", unreal.TextureAddress.TA_CLAMP),
        ):
            try:
                tex.set_editor_property(prop, value)
            except Exception:
                pass

        EAL.save_loaded_asset(tex)
        textures[name] = tex
        log("导入贴图 %s" % name)

    return textures


# ------------------------------------------------------------------ 2. 基础材质
def build_additive_material(texture):
    """
    加法混合 / 无光照。

    Emissive = Texture.RGB × PerInstanceCustomData(0..2) × Color × Intensity

    - 加法混合：亮的东西叠在一起会更亮，符合"能量"的直觉，而且是顺序无关的
      （几十发弹幕同时在屏幕上，半透明排序问题会非常明显，加法直接绕开）
    - PerInstanceCustomData：拖尾靠它做头亮尾淡
    - Color / Intensity：给 C++ 的材质实例动态覆写用（每发弹幕自己的颜色）
    """
    mat = recreate_asset("M_BossSpriteAdd", VFX_DIR, unreal.Material, unreal.MaterialFactoryNew())

    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)

    tex = tex_param(mat, "Texture", texture, -1100, -220)

    cd = MEL.create_material_expression(mat, unreal.MaterialExpressionPerInstanceCustomData3Vector, -1100, 180)
    cd.set_editor_property("data_index", 0)
    cd.set_editor_property("const_default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))

    color = vec_param(mat, "Color", unreal.LinearColor(1.0, 1.0, 1.0, 1.0), -1100, 430)
    intensity = scalar_param(mat, "Intensity", 1.0, -1100, 640)

    node = mul(mat, tex, cd, -700, -160)
    node = mul(mat, node, color, -480, -160)
    node = mul(mat, node, intensity, -260, -160)

    MEL.connect_material_property(node, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)
    log("建立材质 M_BossSpriteAdd（加法混合 / 无光照）")
    return mat


def build_alpha_material(texture):
    """
    半透明 / 无光照，用于贴地的预警圈。

    Emissive = Texture.RGB × Color × Intensity
    Opacity  = Texture.A × Opacity

    预警圈必须用半透明而不是加法：加法会让圈下的地面变成"更亮的地面"，
    而预警圈要的是「一层红色危险区域盖在地上」，需要真实遮挡感。
    """
    mat = recreate_asset("M_BossSpriteAlpha", VFX_DIR, unreal.Material, unreal.MaterialFactoryNew())

    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)

    tex = tex_param(mat, "Texture", texture, -1100, -220)
    color = vec_param(mat, "Color", unreal.LinearColor(1.0, 0.25, 0.2, 1.0), -1100, 260)
    intensity = scalar_param(mat, "Intensity", 1.0, -1100, 470)
    opacity = scalar_param(mat, "Opacity", 1.0, -1100, 680)

    emissive = mul(mat, tex, color, -700, -160)
    emissive = mul(mat, emissive, intensity, -480, -160)
    MEL.connect_material_property(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    # 透明度走贴图的 alpha 通道
    alpha_mul = MEL.create_material_expression(mat, unreal.MaterialExpressionMultiply, -480, 420)
    connect_named(tex, "A", alpha_mul, "A")
    connect_out(opacity, alpha_mul, "B")
    MEL.connect_material_property(alpha_mul, "", unreal.MaterialProperty.MP_OPACITY)

    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)
    log("建立材质 M_BossSpriteAlpha（半透明 / 无光照）")
    return mat


# ------------------------------------------------------------------ 3. 材质实例
def make_instance(name, parent, texture, color, intensity):
    mi = recreate_asset(name, VFX_DIR, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    if not mi or not parent:
        warn("创建材质实例失败：%s" % name)
        return None

    MEL.set_material_instance_parent(mi, parent)
    if texture:
        MEL.set_material_instance_texture_parameter_value(mi, "Texture", texture)
    MEL.set_material_instance_vector_parameter_value(mi, "Color", color)
    MEL.set_material_instance_scalar_parameter_value(mi, "Intensity", intensity)
    MEL.set_material_instance_scalar_parameter_value(mi, "Opacity", 1.0)

    EAL.save_loaded_asset(mi)
    log("建立材质实例 %s" % name)
    return mi


def const_node(mat, value, x, y):
    """建一个常量节点。不同版本的属性名不一样（r / R），都试一遍。"""
    node = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, x, y)
    for prop in ("r", "R", "value"):
        try:
            node.set_editor_property(prop, value)
            return node
        except Exception:
            continue
    warn("常量节点设值失败（值=%s），材质可能偏暗" % value)
    return node


def build_flash_overlay_material():
    """
    起手预警红光 / 失衡绿轮廓：BOSS 身上"烧起来"的轮廓。

    Emissive = Color × Intensity × Flash × (0.06 + 0.94 × Fresnel^2.2)

    - 加法混合：只"加光"、不遮挡 —— BOSS 的动作和细节照常看得清。
    - ⚠⚠ Fresnel 权重是这一版改的（2026-10-04）。原来是 0.4 + 0.6×Fresnel，
      也就是【正面有 40% 的保底亮度】。加上 C++ 每帧把 Intensity 推到 6，
      结果就是整个模型被加法烧成一片亮红、再被 Bloom 糊成一团红雾 ——
      玩家截图里那团"罩在 BOSS 外面、比 BOSS 还大的红色物体"就是它，
      读起来就是"红光没有贴合模型"（其实是模型被自己的光吞掉了）。
      现在保底降到 0.06、并加指数让正中间几乎不亮：
      只有轮廓在烧，模型本体照常看得清 —— 这才是"贴着模型"。
    - used_with_skeletal_mesh：OverlayMaterial 挂在骨骼网格上必须有这一项。
    """
    mat = recreate_asset("M_BossFlashOverlay", VFX_DIR, unreal.Material, unreal.MaterialFactoryNew())

    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    mat.set_editor_property("two_sided", True)
    try:
        mat.set_editor_property("used_with_skeletal_mesh", True)
    except Exception as exc:
        warn("设置 used_with_skeletal_mesh 失败（%s）" % exc)

    color = vec_param(mat, "Color", unreal.LinearColor(1.0, 0.10, 0.06, 1.0), -1100, -260)
    intensity = scalar_param(mat, "Intensity", 3.0, -1100, 40)
    flash = scalar_param(mat, "Flash", 1.0, -1100, 260)

    node = mul(mat, color, intensity, -700, -240)
    node = mul(mat, node, flash, -480, -240)

    # Fresnel 加权：0.06（几乎不亮的保底）+ 0.94 × Fresnel^2.2（只有边缘烧起来）
    try:
        fres = MEL.create_material_expression(mat, unreal.MaterialExpressionFresnel, -760, 380)
        try:
            fres.set_editor_property("exponent", 2.2)
        except Exception:
            pass

        # Fresnel^2.2：再压一次，让正面更干净、边缘更集中
        pow_node = MEL.create_material_expression(mat, unreal.MaterialExpressionPower, -560, 380)
        try:
            pow_node.set_editor_property("exponent", 2.2)
        except Exception:
            pass
        connect_out(fres, pow_node, "A")
        c_expo = const_node(mat, 2.2, -760, 560)
        connect_out(c_expo, pow_node, "B")

        c094 = const_node(mat, 0.94, -760, 700)
        c006 = const_node(mat, 0.06, -760, 860)

        f_mul = mul(mat, pow_node, c094, -340, 380)
        f_add = MEL.create_material_expression(mat, unreal.MaterialExpressionAdd, -160, 430)
        connect_out(f_mul, f_add, "A")
        connect_out(c006, f_add, "B")

        node = mul(mat, node, f_add, -40, -100)
    except Exception as exc:
        warn("Fresnel 分支建立失败（%s）→ 退化为整身均匀泛红" % exc)

    MEL.connect_material_property(node, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)
    log("建立材质 M_BossFlashOverlay（加法混合 / 无光照 / 轮廓红光，保底 0.06）")
    return mat


def make_flash_instance(name, parent, color=None):
    mi = recreate_asset(name, VFX_DIR, unreal.MaterialInstanceConstant,
                        unreal.MaterialInstanceConstantFactoryNew())
    if not mi or not parent:
        warn("创建材质实例失败：%s" % name)
        return None

    # 默认红色 = 起手预警；失衡态传绿色进来（参考 Demo 的 UNSTABLE 绿色轮廓）
    if color is None:
        color = unreal.LinearColor(1.0, 0.10, 0.06, 1.0)

    MEL.set_material_instance_parent(mi, parent)
    MEL.set_material_instance_vector_parameter_value(mi, "Color", color)
    MEL.set_material_instance_scalar_parameter_value(mi, "Intensity", 3.0)
    MEL.set_material_instance_scalar_parameter_value(mi, "Flash", 1.0)

    EAL.save_loaded_asset(mi)
    log("建立材质实例 %s" % name)
    return mi


def main():
    log("开始搭建 BOSS 特效材质（源：%s）" % SOURCE_DIR)

    textures = import_textures()
    if not textures:
        warn("没有导入任何贴图，脚本终止")
        return

    glow_tex = textures.get("T_VFX_Glow")
    impact_tex = textures.get("T_VFX_Impact")
    ring_tex = textures.get("T_VFX_Ring")
    telegraph_tex = textures.get("T_VFX_Telegraph")

    add_mat = build_additive_material(glow_tex)
    alpha_mat = build_alpha_material(telegraph_tex)

    # 默认颜色只是兜底：C++ 会在运行时用材质实例（MID）按招式覆写
    make_instance("MI_BossGlow", add_mat, glow_tex, unreal.LinearColor(0.35, 0.72, 1.0, 1.0), 1.0)
    make_instance("MI_BossImpact", add_mat, impact_tex, unreal.LinearColor(0.75, 0.90, 1.0, 1.0), 1.0)
    make_instance("MI_BossRing", add_mat, ring_tex, unreal.LinearColor(0.40, 0.75, 1.0, 1.0), 1.0)
    make_instance("MI_BossTelegraph", alpha_mat, telegraph_tex, unreal.LinearColor(1.0, 0.22, 0.15, 1.0), 1.0)

    # 起手预警红光（BOSS 出招前全身泛红）：给 C++ 当 OverlayMaterial 用
    flash_mat = build_flash_overlay_material()
    make_flash_instance("MI_BossFlash", flash_mat)
    # 失衡（UNSTABLE）态的绿色轮廓：异常条打满时 C++ 会把它挂到 Mesh 的 OverlayMaterial 上
    make_flash_instance("MI_BossUnstable", flash_mat, unreal.LinearColor(0.20, 1.0, 0.45, 1.0))
    # 举盾态的冷蓝轮廓 + 护体光配色：BOSS 默认格挡时 C++ 挂到 OverlayMaterial
    make_flash_instance("MI_BossGuard", flash_mat, unreal.LinearColor(0.45, 0.72, 1.0, 1.0))

    log("完成。C++ 会按这些路径加载：")
    log("    /Game/Boss/VFX/MI_BossGlow.MI_BossGlow        （弹体核心球 + 拖尾）")
    log("    /Game/Boss/VFX/MI_BossImpact.MI_BossImpact    （命中爆闪）")
    log("    /Game/Boss/VFX/MI_BossRing.MI_BossRing        （命中冲击环）")
    log("    /Game/Boss/VFX/MI_BossTelegraph.MI_BossTelegraph（贴地预警圈）")
    log("    /Game/Boss/VFX/MI_BossFlash.MI_BossFlash    （起手预警：BOSS 全身闪红光）")


if __name__ == "__main__":
    main()
