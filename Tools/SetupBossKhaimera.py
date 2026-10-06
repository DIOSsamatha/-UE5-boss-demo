# -*- coding: utf-8 -*-
"""
SetupBossKhaimera.py
====================
把 BOSS 换成 Paragon 的 Khaimera（模型 + 动画 + 动画蓝图），
**不需要改任何 C++，也不需要重新编译**。

原理
----
C++（UBossBrainComponent::EnsureDefaultPhases）是按【约定路径】去加载蒙太奇的：
    /Game/Boss/Animation/Boss_Slash_01_Montage ... 等 25 个
蒙太奇是【绑骨架】的资产 —— 目前这批是 SwordAnimsetPro（UE4 Mannequin 骨架）生成的，
直接把网格换成 Khaimera（Khaimera_Skeleton）的话，这些蒙太奇一个都播不了。

所以本脚本做的事是：
    用 **Khaimera 自己的动画**，把那 25 个蒙太奇【重新生成一遍，名字和路径完全不变】
    → 新蒙太奇绑在 Khaimera_Skeleton 上 → C++ 照旧加载，零改动。
同时可选把 BP_Boss 的网格 / 动画蓝图指到 Khaimera。

招式总览（30 个蒙太奇，C++ 侧招式池一共 12 招）
------------------------------------------------
  近战·标准三连   Slash_01/02/03  <- Melee_A / B / C
  近战·快爪三连   Slash_04/05/06  <- Melee_A/B/C_Fast     （二阶段解锁，快、收招短）
  近战·重砸三连   Heavy_01/02/03  <- Melee_A/B/C_Slow     （慢、重、前摇长，最该弹的一类）
  突进斩         ChargeSlash     <- E_Ability_Start + E_Ability_Throw（长距离贴地冲，两段判定）
  腾空扑击       Pounce          <- Melee_Air_Attack + Bound（中距离起跳扑，落地一段判定）
  大招           Ultimate        <- R_Ability 三件套
三档速度（标准 / Fast / Slow）是"招式多样化"的关键：快招逼反应，慢招逼判断。

为什么根位移默认关
------------------
Paragon 的角色动画位移是靠代码推的（本工程的突进斩也是"位移由代码推，不靠根位移"）。
保留根位移会让 BOSS 出招时莫名其妙滑动一段；关掉后位移完全由 C++ 掌控，可预测。
想让某一招"带位移冲出去"的话，把下面对应条目的最后一个参数改成 True。

用法：编辑器 Output Log 右上角切到 Python，输入框填绝对路径回车：
      K:/selfgame/demo/Tools/SetupBossKhaimera.py
"""

import unreal

ASSET_LIB = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

OUT_DIR = "/Game/Boss/Animation"
SRC = "/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations"

# ---------------------------------------------------------------- 映射表
# (目标蒙太奇名, 源动画, 混合进, 混合出, 是否保留根位移)
#
# 名字必须和 C++ 约定路径【完全一致】，改名字 = C++ 找不到 = 该招退化成空动作。
# 想换某一招的观感，只改“源动画”这一列即可，其余照抄。
MONTAGES = [
    # ---- 近战六连：Khaimera 有 A/B/C 三套普攻，各有 Fast / Slow 变体 ----
    ("Boss_Slash_01_Montage",      "Melee_A",            0.10, 0.16, False),
    ("Boss_Slash_02_Montage",      "Melee_B",            0.08, 0.16, False),
    ("Boss_Slash_03_Montage",      "Melee_C",            0.12, 0.20, False),
    # ⚠ 04/05/06 必须【同一套变体】：混着用（比如把 Slow 塞进 06）会让"快速三连"
    #   的节奏断掉 —— 前两下很快、第三下突然变成慢起手，观感像卡帧。
    ("Boss_Slash_04_Montage",      "Melee_A_Fast",       0.10, 0.18, False),   # 快三连 1
    ("Boss_Slash_05_Montage",      "Melee_B_Fast",       0.10, 0.20, False),   # 快三连 2
    ("Boss_Slash_06_Montage",      "Melee_C_Fast",       0.10, 0.22, False),   # 快三连 3

    # ---- 重砸三连：Slow 变体（摆手极大、前摇长 → 最该被弹反的一类招）----
    # ⚠ 这三招的判定窗口在 C++ 里被往后挪到 0.34~0.64（快招是 0.22~0.50），
    #   因为 Slow 变体"抬起上半身"那半段刀还没落下来，照快招的窗口会出现"挥到一半就打完"。
    ("Boss_Heavy_01_Montage",      "Melee_A_Slow",        0.12, 0.22, False),   # 重砸 1（一阶段就有）
    ("Boss_Heavy_02_Montage",      "Melee_B_Slow",        0.12, 0.22, False),   # 重砸 2 横扫（够得最远）
    ("Boss_Heavy_03_Montage",      "Melee_C_Slow",        0.14, 0.26, False),   # 重砸 3 收招（最重、后摇最长）

    # ---- 腾空扑击：中距离专用（突进斩嫌远、近战嫌近的那段距离带）----
    # Dash = 起跳扑过去（位移仍由 C++ 推，根位移关），Impact = 落地砸。
    ("Boss_Pounce_Dash_Montage",   "Melee_Air_Attack",    0.06, 0.12, False),
    ("Boss_Pounce_Impact_Montage", "Bound",               0.06, 0.24, False),   # Bound = 扑跃落地

    # ---- 远程弹幕：施法抬手 ----
    ("Boss_Ranged_Cast_Montage",   "Cast",               0.12, 0.18, False),

    # ---- 突进斩：冲刺 + 收尾重斩（位移由 C++ 推，根位移必须关）----
    # ⚠⚠ 原用 E_Ability_Start，但它是【加法动画】(AAT_RotationOffsetMeshSpace)，
    #   单节点会直接拒绝 → 突进斩的冲刺段根本没有动作（玩家只看到 BOSS 平移过去）。
    #   换成 TravelMode_Fwd（疾行前冲，已确认非加法）：观感上正是"压低身子往前冲"。
    ("Boss_Charge_Dash_Montage",   "TravelMode_Fwd",     0.05, 0.10, False),
    ("Boss_Charge_Impact_Montage", "E_Ability_Throw",    0.06, 0.22, False),

    # ---- 大招三件套 ----
    ("Boss_Ult_Charge_Montage",    "R_Ability_Intro",     0.12, 0.16, False),
    ("Boss_Ult_Loop_Montage",      "R_Ability_Intro_Loop", 0.10, 0.10, False),
    ("Boss_Ult_Out_Montage",       "R_Ability",           0.10, 0.18, False),

    # ---- 回旋横扫（2026-09-30 新增）----
    # 为什么必须有它：Melee_A / Melee_A_Fast / Melee_A_Slow 本质是【同一个挥击动作的三个速度版本】，
    #   玩家眼里 Slash_01、Slash_04、Heavy_01 就是"同一个挥砍，只是快中慢" → 招式显得单一。
    #   Spin_Bwd2Fwd 是"转体一圈"，和任何直线挥砍的观感都不一样，
    #   这才叫多了一个【新招式】，而不是多了一个速度档。
    ("Boss_Spin_01_Montage",       "Spin_Bwd2Fwd_CW",     0.10, 0.22, False),   # 回旋扫（顺时针）
    ("Boss_Spin_02_Montage",       "Spin_Bwd2Fwd_CWW",    0.10, 0.22, False),   # 回旋扫（逆时针，二阶段）

    # ---- 投掷 / 大幅挥出（2026-09-30 新增）----
    # ⚠ 最初选了 E_Ability_Throw_MSA，但它和其他 *_MSA 一样是【加法动画】→ 播不出来。
    #   改用 RMB_60fps（远程挥击，非加法）：动作幅度大、手抬得高、收招慢，
    #   观感上和"贴身挥砍""突进""扑击""回旋"四者都不重叠 → 第五类动作。
    ("Boss_Throw_01_Montage",      "RMB_60fps",           0.08, 0.24, False),

    # ---- 二阶段狂化：嚎叫（Emote_Taunt_Howl）非常适合当"狂化"演出 ----
    ("Boss_Phase2_Enter_Montage",  "Emote_Taunt_Howl_T1", 0.15, 0.15, False),
    # ⚠ 原用 Idle_Relaxed_B，但它是【加法动画】(AAT_LocalSpaceBase) → 二阶段循环播不出来。
    #   换成 HS_Idle_Relaxed_A（已确认非加法）。
    ("Boss_Phase2_Loop_Montage",   "HS_Idle_Relaxed_A",   0.12, 0.12, False),
    ("Boss_Phase2_Out_Montage",    "LevelStart",          0.10, 0.20, False),

    # ---- 四向受击 ----
    # ⚠⚠ Khaimera 的 HitReact_Front/Back/Left/Right 【四个全是加法动画】
    #   (AAT_LocalSpaceBase) → 单节点全部拒绝 → BOSS 挨打时一点反应动作都没有。
    #   之前一直以为"四向受击 1:1 对上了"，实际四个都播不出来。
    #   换成已确认非加法的替代动作：
    #     前/后被击退用 Knockback_Front / Knockback_Back（真的是"被打得后退"），
    #     左/右用 Melee_*_Recovery（收招动作，短促，适合当侧向受击的抖动）。
    ("Boss_Hit_F_Montage",         "Knockback_Front",    0.05, 0.12, False),
    ("Boss_Hit_B_Montage",         "Knockback_Back",     0.05, 0.12, False),
    ("Boss_Hit_L_Montage",         "Melee_A_Recovery",   0.05, 0.14, False),
    ("Boss_Hit_R_Montage",         "Melee_B_Recovery",   0.05, 0.14, False),

    # ---- 破防 / 虚弱：Stunned 三件套正好对应 进入 / 循环 / 恢复 ----
    ("Boss_Weak_In_Montage",       "Stunned_Start",      0.10, 0.15, False),
    ("Boss_Weak_Loop_Montage",     "Stunned_Loop",       0.12, 0.12, False),
    ("Boss_Weak_Recover_Montage",  "Stunned_End",        0.10, 0.18, False),

    # ---- 被弹反的大硬直：击退 ----
    ("Boss_Stagger_Montage",       "Knockback_Front",    0.06, 0.15, False),
    ("Boss_Death_Montage",         "Death_A",            0.10, 0.30, False),

    # ---- 举盾/架势姿态：Khaimera 没有举盾动作，用待机顶上（循环）----
    # ⚠ 原用 Idle_Relaxed_A，但它是【加法动画】→ 单节点拒绝 → 举盾姿势播不出来
    #   （C++ 侧因此一直走"跳过举盾、只用护体光"的降级分支）。
    #   换成 HS_Idle（已确认非加法）后举盾姿势才真的有动作。
    ("Boss_Guard_Idle_Montage",    "HS_Idle",            0.20, 0.20, False),
]

LOOP_MONTAGES = {"Boss_Guard_Idle_Montage"}

HIT_REACT_NAMES = [
    "Boss_Hit_F_Montage", "Boss_Hit_B_Montage",
    "Boss_Hit_L_Montage", "Boss_Hit_R_Montage",
]

# ---------------------------------------------------------------- BP_Boss 切换
BP_BOSS = "/Game/Boss/System/BP_Boss"
KHAIMERA_MESH = "/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Meshes/Khaimera"
KHAIMERA_ANIMBP = "/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Khaimera_AnimBlueprint"
# 是否顺带把 BP_Boss 的网格 / 动画蓝图切过去。只想先换动画的话设 False。
# ⚠⚠ 必须保持 False（2026-09-30 定案）
#
# 打开它会把 BP_Boss 的【动画蓝图】也指到 Khaimera 包自带的那个 *_AnimBlueprint。
# 但那个 ABP 是 Paragon 示例 PlayerCharacter 专用的，不是通用动画蓝图：
#   它会自己 Montage_Play 一个 LevelStart、依赖一堆英雄状态变量 →
#   结果是满屏 Divide by zero、BOSS 整体不动。
#
# 现在 BOSS 走的是【单节点模式 + C++ 直接 PlayAnimation】这条已跑通的路，
# 一旦被换成第三方 ABP，整套动画系统会一起崩。
# 本脚本的用途只剩"按约定路径重新生成蒙太奇"，不要再顺手改 BP 的动画类。
# （真要换网格，请在编辑器里手动改，别改动画类。）
APPLY_BP_MESH = False


def _set_root_motion(asset, path, label, want):
    """按 want 设置 Enable Root Motion（源动画 + 蒙太奇都设一遍）。"""
    if asset is None:
        return
    try:
        before = asset.get_editor_property("enable_root_motion")
    except Exception:
        return
    if before == want:
        return
    try:
        asset.set_editor_property("enable_root_motion", want)
        ASSET_LIB.save_asset(path)
        unreal.log(f"[RM] {label}: {before} -> {want}")
    except Exception as exc:
        unreal.log_warning(f"[RM] {label} 设置失败（不影响功能）：{exc}")


def _tune_blend(montage, name, in_time, out_time):
    """UE 各版本属性名不一致（blend_in / blend_in_time），逐个尝试。"""
    for prop, value in (("blend_in", in_time), ("blend_out", out_time)):
        try:
            blend = montage.get_editor_property(prop)
            blend.set_editor_property("blend_time", value)
            montage.set_editor_property(prop, blend)
            continue
        except Exception:
            pass
        try:
            montage.set_editor_property(f"{prop}_time", value)
        except Exception:
            pass


def create_montage(anim_name, montage_name, in_time, out_time, root_motion, loop=False):
    seq_path = f"{SRC}/{anim_name}"
    full = f"{OUT_DIR}/{montage_name}"

    seq = ASSET_LIB.load_asset(seq_path)
    if seq is None:
        unreal.log_error(f"[SKIP] 找不到源动画 {seq_path} → 跳过 {montage_name}")
        return None

    _set_root_motion(seq, seq_path, f"源动画 {anim_name}", root_motion)

    # 强制重建：改了映射表重跑就生效
    if ASSET_LIB.does_asset_exist(full):
        ASSET_LIB.delete_asset(full)

    try:
        factory = unreal.AnimMontageFactory()
        factory.set_editor_property("target_skeleton", seq.get_editor_property("skeleton"))
        factory.set_editor_property("source_animation", seq)
        montage = TOOLS.create_asset(montage_name, OUT_DIR, unreal.AnimMontage, factory)
    except Exception as exc:
        unreal.log_error(f"[FAIL] 创建 {montage_name} 出错：{exc}")
        return None

    if montage is None:
        unreal.log_error(f"[FAIL] 创建 {montage_name} 返回空")
        return None

    _set_root_motion(montage, full, f"蒙太奇 {montage_name}", root_motion)
    _tune_blend(montage, montage_name, in_time, out_time)

    if loop:
        try:
            montage.set_editor_property("loop_setting", unreal.MontageLoopSetting.ENABLED)
        except Exception as exc:
            unreal.log_warning(f"[Loop] {montage_name} 设置循环失败：{exc}")

    ASSET_LIB.save_asset(full)

    length = 0.0
    try:
        length = montage.get_editor_property("sequence_length")
    except Exception:
        pass
    unreal.log(f"[OK] {montage_name}  时长≈{length:.2f}s  源={anim_name}")
    return montage


def apply_bp_mesh():
    """把 BP_Boss 的骨骼网格 / 动画蓝图切到 Khaimera。"""
    bp = ASSET_LIB.load_asset(BP_BOSS)
    if bp is None:
        unreal.log_error(f"[BP] 找不到 {BP_BOSS}")
        return

    mesh_asset = ASSET_LIB.load_asset(KHAIMERA_MESH)
    animbp = ASSET_LIB.load_asset(KHAIMERA_ANIMBP)
    if mesh_asset is None:
        unreal.log_error(f"[BP] 找不到网格 {KHAIMERA_MESH}")
        return
    if animbp is None:
        unreal.log_warning(f"[BP] 找不到动画蓝图 {KHAIMERA_ANIMBP}（只换网格）")

    try:
        cdo = unreal.get_default_object(bp.generated_class())
    except Exception as exc:
        unreal.log_error(f"[BP] 取不到 CDO：{exc}")
        return

    try:
        mesh_comp = cdo.get_editor_property("mesh")
    except Exception as exc:
        unreal.log_error(f"[BP] 取不到 Mesh 组件：{exc}")
        return

    # UE5 里这个属性叫 skeletal_mesh_asset，UE4 叫 skeletal_mesh —— 都试一遍
    for prop in ("skeletal_mesh_asset", "skeletal_mesh"):
        try:
            mesh_comp.set_editor_property(prop, mesh_asset)
            unreal.log(f"[BP] 网格已设为 Khaimera（属性 {prop}）")
            break
        except Exception:
            continue

    if animbp is not None:
        # ⚠ 不再把 Khaimera_AnimBlueprint 绑到 BOSS 上：
        # 那是【玩家角色】的 ABP，内部依赖英雄状态变量 → 除以零 → 整只 BOSS 完全不动 / 永远 T-Pose。
        # BOSS 的动画现在完全由 C++（单节点待机 + Jog + 蒙太奇）驱动，见 BossCharacterBase::SetupBossAnimation。
        # 这里反而把 anim_class 显式清空，避免残留的坏 ABP 在运行时被实例化。
        try:
            mesh_comp.set_editor_property("anim_class", None)
            unreal.log_warning(
                "[BP] 动画蓝图已【清空】（不再用 Khaimera_AnimBlueprint）。BOSS 动画由 C++ 单节点模式驱动，"
                "无需任何 AnimBlueprint。")
        except Exception as exc:
            unreal.log_warning(f"[BP] anim_class 清空失败（不影响功能，C++ 会强制单节点模式）：{exc}")

    # 只报告当前 Transform，不擅自改 —— 换模型后如果浮空/陷地，按下面的提示手动调
    try:
        loc = mesh_comp.get_editor_property("relative_location")
        rot = mesh_comp.get_editor_property("relative_rotation")
        scale = mesh_comp.get_editor_property("relative_scale3d")
        unreal.log_warning(
            f"[BP] 当前 Mesh Transform：位置={loc} 旋转={rot} 缩放={scale}\n"
            "      换模型后如果出现【浮空 / 陷进地面 / 朝向不对】，改这三个值：\n"
            "      · 浮空/陷地 → 调位置的 Z（胶囊半高 130，通常取 -130 左右）\n"
            "      · 朝向不对   → 旋转 Yaw 设 -90（UE4 角色通用朝向修正）\n"
            "      · 大小不对   → 缩放（Khaimera 体型偏大，一般 1.0，不够再放大）"
        )
    except Exception:
        pass

    try:
        ASSET_LIB.save_asset(BP_BOSS)
        unreal.log(f"[BP] 已保存 {BP_BOSS}")
    except Exception as exc:
        unreal.log_warning(f"[BP] 保存失败：{exc}")


def verify():
    unreal.log_warning("========== 复核：C++ 约定路径 ==========")
    missing = []
    for name, _, _, _, _ in MONTAGES:
        if ASSET_LIB.does_asset_exist(f"{OUT_DIR}/{name}"):
            unreal.log(f"  [存在] {name}")
        else:
            missing.append(name)
            unreal.log_error(f"  [缺失] {name}")

    hit_ok = all(ASSET_LIB.does_asset_exist(f"{OUT_DIR}/{n}") for n in HIT_REACT_NAMES)
    unreal.log_warning(f"四向受击蒙太奇完整：{hit_ok}")

    if missing:
        unreal.log_warning(f"共缺失 {len(missing)} 个：{missing}")
    else:
        unreal.log_warning(f"全部 {len(MONTAGES)} 个蒙太奇就位。")

    # 武器插槽体检：Khaimera 是徒手/爪击，插槽要加在【角色骨骼网格】上
    try:
        cdo = unreal.get_default_object(ASSET_LIB.load_asset(BP_BOSS).generated_class())
        mesh_comp = cdo.get_editor_property("mesh")
        sk = None
        for prop in ("skeletal_mesh_asset", "skeletal_mesh"):
            try:
                sk = mesh_comp.get_editor_property(prop)
                if sk:
                    break
            except Exception:
                continue
        if sk:
            has_s = sk.get_editor_property("skeleton") is not None
            unreal.log_warning(
                f"[插槽] 当前网格：{sk.get_name()}（骨架："
                f"{sk.get_editor_property('skeleton').get_name() if has_s else '?'}）\n"
                "       近战判定需要 WeaponStart / WeaponEnd 两个插槽。Khaimera 是徒手攻击，\n"
                "       请在骨骼上补这两个插槽（见脚本尾部的编辑器步骤），否则判定会退回\n"
                "       \"身前 40~120cm\" 粗判，出现\"看着砍中不判、没碰到反而判\"。"
            )
    except Exception as exc:
        unreal.log(f"[插槽] 体检跳过：{exc}")
    unreal.log_warning("=======================================")


def main():
    unreal.log_warning("========== SetupBossKhaimera 开始 ==========")

    if not ASSET_LIB.does_directory_exist(OUT_DIR):
        ASSET_LIB.make_directory(OUT_DIR)

    created = []
    for name, anim, in_t, out_t, rm in MONTAGES:
        m = create_montage(anim, name, in_t, out_t, rm, name in LOOP_MONTAGES)
        if m is not None:
            created.append(name)

    unreal.log_warning(f"成功创建 {len(created)}/{len(MONTAGES)} 个蒙太奇")

    if APPLY_BP_MESH:
        apply_bp_mesh()

    verify()

    unreal.log_warning(
        "========== 还差一步（近战判定）==========\n"
        "Khaimera 徒手攻击，需要给它的骨骼加两个插槽，否则近战判定和动作脱节：\n"
        "  1) 打开 Content/ParagonKhaimera/Characters/Heroes/Khaimera/Meshes/Khaimera\n"
        "  2) 切到 Skeleton（骨骼）标签\n"
        "  3) 在右手骨骼（hand_r 一类）上右键 → Add Socket，命名 WeaponStart（刀根/手心）\n"
        "  4) 同样再加一个 WeaponEnd（指尖/攻击前端）\n"
        "  5) 选中 WeaponStart，在 Details 里把预览网格选成武器（没有武器就空着，位置对就行）\n"
        "  6) 保存资产\n"
        "验证：运行时日志应出现 WeaponStart=找到, WeaponEnd=找到\n"
        "========================================="
    )


main()
