# -*- coding: utf-8 -*-
"""
修 BOSS 特效材质「编译失败 → 引擎回退默认材质」的问题（在 UE 编辑器里跑）

## 症状
运行时日志出现：
    LogMaterial: Warning: [AssetLog] .../M_BossSpriteAdd.uasset:
        Failed to compile Material for platform PCD3D_SM6, Default Material will be used in game.
后果：所有用到它的东西（弹体核心球 / 命中爆闪 / 起手爆闪 / 拖尾）全部变成
      **默认材质的深灰球** —— 就是"BOSS 头上/身边凭空出现一个黑球"的观感来源之一。

## 为什么
M_BossSpriteAdd 是 Tools/SetupBossVFX.py 用 Python 现搭的图（加法混合 + 无光照 +
PerInstanceCustomData3Vector）。这类"脚本搭出来的材质"常见情况是：
**保存时 shader 还没编完 / DDC 里留了半成品**，下次启动就报"编译失败"并回退默认材质。
另一个已知坑：材质用了 PerInstanceCustomData（拖尾靠它做头亮尾淡），
但没勾 `used_with_instanced_static_meshes` → ISM 上的每实例自定义数据不生效。

## 本脚本做什么（都是公开的编辑器 API，不动图结构）
1) 把 M_BossSpriteAdd / M_BossSpriteAlpha 的 `used_with_instanced_static_meshes` 补上
2) 强制 **重编译 + 重新保存**（这一步就能清掉"半成品 shader"导致的失败）
3) 顺手把 4 个材质实例也重编译 + 保存
4) 打印每个资产的 status，方便确认

## 跑法（编辑器内）
    Output Log → 切 Python 模式：
    exec(open(r"K:\\selfgame\\demo\\Tools\\FixBossVFXMaterial.py", encoding="utf-8").read())

## 跑完之后
重启 PIE。日志里应该不再出现 "M_BossSpriteAdd ... Failed to compile Material"。
如果**仍然**报，把 Material Editor 里打开 M_BossSpriteAdd 时上方提示的那条
**具体错误**（例如 "Missing MaterialParameterCollection xxx"）发我，我针对性改图。
"""

import sys

try:
    import unreal
except ImportError:
    print("[X] 必须在 UE 编辑器的 Python 环境里跑")
    sys.exit(1)

VFX_DIR = "/Game/Boss/VFX"

EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary

BASE_MATERIALS = ["M_BossSpriteAdd", "M_BossSpriteAlpha"]
INSTANCES = ["MI_BossGlow", "MI_BossImpact", "MI_BossRing", "MI_BossTelegraph"]


def log(msg):
    unreal.log("[VFXFix] " + msg)


def warn(msg):
    unreal.log_warning("[VFXFix] " + msg)


def fix_base(name):
    full = "{}/{}".format(VFX_DIR, name)
    if not EAL.does_asset_exist(full):
        warn("找不到 %s（先跑 SetupBossVFX.py）" % full)
        return

    mat = EAL.load_asset(full)
    if not mat:
        warn("加载失败：%s" % full)
        return

    # 拖尾是 InstancedStaticMesh + PerInstanceCustomData，必须打开这个开关，
    # 否则每实例颜色不生效（表现：整条尾巴一样亮，没有头亮尾淡）。
    for prop in ("used_with_instanced_static_meshes",):
        try:
            mat.set_editor_property(prop, True)
        except Exception as exc:
            warn("%s 设置 %s 失败：%s" % (name, prop, exc))

    try:
        mat.set_editor_property("two_sided", True)
    except Exception:
        pass

    # 关键一步：强制重编译（清掉半成品 shader），再保存
    ok = False
    try:
        ok = MEL.recompile_material(mat)
    except Exception as exc:
        warn("%s 重编译抛异常：%s" % (name, exc))

    try:
        EAL.save_loaded_asset(mat)
    except Exception as exc:
        warn("%s 保存失败：%s" % (name, exc))

    log("%s 已重编译（recompile_material=%s）并保存" % (name, ok))


def fix_instance(name):
    full = "{}/{}".format(VFX_DIR, name)
    if not EAL.does_asset_exist(full):
        warn("找不到材质实例 %s" % full)
        return
    mi = EAL.load_asset(full)
    if not mi:
        warn("加载材质实例失败：%s" % full)
        return
    try:
        MEL.recompile_material(mi)
    except Exception:
        pass
    try:
        EAL.save_loaded_asset(mi)
    except Exception as exc:
        warn("%s 保存失败：%s" % (name, exc))
    log("%s 已重编译并保存" % name)


def main():
    log("开始修复 BOSS 特效材质编译失败问题")
    for m in BASE_MATERIALS:
        fix_base(m)
    for mi in INSTANCES:
        fix_instance(mi)
    log("完成。重启 PIE 后检查日志里是否还有 'M_BossSpriteAdd ... Failed to compile Material'")


if __name__ == "__main__":
    main()
