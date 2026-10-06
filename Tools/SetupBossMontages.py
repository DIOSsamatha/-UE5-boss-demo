# -*- coding: utf-8 -*-
"""
SetupBossMontages.py
====================
按 SwordAnimsetPro 里的源动画，为 BOSS 生成一整套蒙太奇（放在 /Game/Boss/Animation/）。

C++ 侧（UBossBrainComponent::EnsureDefaultPhases）是按【约定路径】去加载这些蒙太奇的，
所以只要跑完这个脚本，BOSS 立刻就有动作了 —— 不需要在 BP_Boss 里手动拖任何资产。

生成的清单（左侧 = Boss 用的名字，右侧 = 取材的源动画）：
    Boss_Slash_01_Montage       <- Root-Motion/Attack_Combo_02_01_Anim   近战挥砍第 1 段
    Boss_Slash_02_Montage       <- Root-Motion/Attack_Combo_02_02_Anim   近战挥砍第 2 段（连招链）
    Boss_Slash_03_Montage       <- Root-Motion/Attack_Combo_03_01_Anim   近战重击（二阶段）
    Boss_Slash_04_Montage       <- Root-Motion/Attack_Combo_01_01_Anim   横扫（覆盖面广）
    Boss_Slash_05_Montage       <- Root-Motion/Attack_Combo_01_02_Anim   突进挑斩（贴脸高伤）
    Boss_Slash_06_Montage       <- Root-Motion/Attack_Rising_Anim        重砸（慢起手大硬直窗口）
    Boss_Ranged_Cast_Montage    <- In-Place/Buff_In_Anim                 远程弹幕的施法抬手
    Boss_Charge_Dash_Montage    <- Root-Motion/Attack_Sprint_Anim        突进斩：贴地冲刺（位移由代码推，不靠根位移）
    Boss_Charge_Impact_Montage  <- Root-Motion/Attack_Combo_03_02_Anim   突进斩：收尾重斩
    Boss_Ult_Charge_Montage     <- Root-Motion/Block_Breach_Anim         大招蓄力起手
    Boss_Ult_Loop_Montage       <- In-Place/Buff_Loop_Anim               大招蓄力循环
    Boss_Ult_Out_Montage        <- In-Place/Buff_out_Anim                大招收力
    Boss_Phase2_Enter_Montage   <- In-Place/Crouch_In_Anim               二阶段狂化：蹲下
    Boss_Phase2_Loop_Montage    <- In-Place/Buff_Loop_Anim               二阶段狂化：蓄势
    Boss_Phase2_Out_Montage     <- In-Place/Buff_out_Anim                二阶段狂化：爆发起身
    Boss_Hit_F/B/L/R_Montage    <- Root-Motion/Hit_01_F/B/L/R_Anim       四向受击
    Boss_Weak_In_Montage        <- Root-Motion/Weak_01_In_Anim           破防：进入虚弱
    Boss_Weak_Loop_Montage      <- In-Place/Weak_01_loop_01_Anim         破防：虚弱待机
    Boss_Weak_Recover_Montage   <- Root-Motion/Weak_01_Recovery_Anim     破防：恢复
    Boss_Stagger_Montage        <- Root-Motion/Hit_Drop_03_Start_Anim    被弹反的大硬直
    Boss_Death_Montage          <- Root-Motion/Dead_Anim                 死亡
    Boss_Guard_Idle_Montage     <- In-Place/Block_Hold_Anim              举盾姿态（循环，站着挡时播）

用法：编辑器 Output Log 右上角切到 Python，输入框填绝对路径回车：
      K:/selfgame/demo/Tools/SetupBossMontages.py
（也可整段粘贴运行，无需重编 C++）
"""

import unreal

ASSET_LIB = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

OUT_DIR = "/Game/Boss/Animation"
SRC_RM = "/Game/SwordAnimsetPro/Animations/Root-Motion"
SRC_IP = "/Game/SwordAnimsetPro/Animations/In-Place"

# 已经废弃的蒙太奇：原先的「跳砸」三件套。招式已换成突进斩，这里顺手清掉，
# 免得 Content Browser 里留一堆再也用不到、还容易被误拖到蓝图上的资产。
RETIRED = [
    "Boss_Leap_In_Montage",
    "Boss_Leap_Air_Montage",
    "Boss_Leap_Land_Montage",
]

# 这些蒙太奇要设成"循环"（举盾姿态：站着挡时一直播，停盾时由 C++ 用混合出时间收掉）
LOOP_MONTAGES = {
    "Boss_Guard_Idle_Montage",
}

# (目标蒙太奇名, 源动画路径, 混合进, 混合出)
# 混合时间越短 = 动作切换越干脆；BOSS 的攻击要"重"，所以比玩家略长一点点。
MONTAGES = [
    ("Boss_Slash_01_Montage",     f"{SRC_RM}/Attack_Combo_02_01_Anim",   0.10, 0.16),
    ("Boss_Slash_02_Montage",     f"{SRC_RM}/Attack_Combo_02_02_Anim",   0.08, 0.16),
    ("Boss_Slash_03_Montage",     f"{SRC_RM}/Attack_Combo_03_01_Anim",   0.12, 0.20),
    # 三套新近战（C++ 的 EnsureDefaultPhases 会去招式池里找它们，缺了只是警告 + 空动作）
    ("Boss_Slash_04_Montage",     f"{SRC_RM}/Attack_Combo_01_01_Anim",   0.12, 0.18),
    ("Boss_Slash_05_Montage",     f"{SRC_RM}/Attack_Combo_01_02_Anim",   0.10, 0.20),
    ("Boss_Slash_06_Montage",     f"{SRC_RM}/Attack_Rising_Anim",        0.14, 0.24),
    ("Boss_Ranged_Cast_Montage",  f"{SRC_IP}/Buff_In_Anim",              0.12, 0.18),
    # 突进斩：冲刺段混合时间给到最短 —— 它是"瞬间爆出去"的动作，
    # 混太久会变成"慢慢加速"，反而丢掉突进感；收尾斩相反，混合长一点收得住。
    ("Boss_Charge_Dash_Montage",  f"{SRC_RM}/Attack_Sprint_Anim",        0.05, 0.10),
    ("Boss_Charge_Impact_Montage", f"{SRC_RM}/Attack_Combo_03_02_Anim",  0.06, 0.22),
    ("Boss_Ult_Charge_Montage",   f"{SRC_RM}/Block_Breach_Anim",         0.12, 0.16),
    ("Boss_Ult_Loop_Montage",     f"{SRC_IP}/Buff_Loop_Anim",            0.10, 0.10),
    ("Boss_Ult_Out_Montage",      f"{SRC_IP}/Buff_out_Anim",             0.10, 0.18),
    ("Boss_Phase2_Enter_Montage", f"{SRC_IP}/Crouch_In_Anim",            0.15, 0.15),
    ("Boss_Phase2_Loop_Montage",  f"{SRC_IP}/Buff_Loop_Anim",            0.12, 0.12),
    ("Boss_Phase2_Out_Montage",   f"{SRC_IP}/Buff_out_Anim",             0.10, 0.20),
    ("Boss_Hit_F_Montage",        f"{SRC_RM}/Hit_01_F_Anim",             0.05, 0.12),
    ("Boss_Hit_B_Montage",        f"{SRC_RM}/Hit_01_B_Anim",             0.05, 0.12),
    ("Boss_Hit_L_Montage",        f"{SRC_RM}/Hit_01_L_Anim",             0.05, 0.12),
    ("Boss_Hit_R_Montage",        f"{SRC_RM}/Hit_01_R_Anim",             0.05, 0.12),
    ("Boss_Weak_In_Montage",      f"{SRC_RM}/Weak_01_In_Anim",           0.10, 0.15),
    ("Boss_Weak_Loop_Montage",    f"{SRC_IP}/Weak_01_loop_01_Anim",      0.12, 0.12),
    ("Boss_Weak_Recover_Montage", f"{SRC_RM}/Weak_01_Recovery_Anim",     0.10, 0.18),
    ("Boss_Stagger_Montage",      f"{SRC_RM}/Hit_Drop_03_Start_Anim",    0.06, 0.15),
    ("Boss_Death_Montage",        f"{SRC_RM}/Dead_Anim",                 0.10, 0.30),
    # 举盾姿态：取自 Block_Hold（举刀架住的待机），循环播放。混合时间给长一点（0.20），
    # 让"抬起盾"和"放下盾"都平滑，不要"啪"地出现/消失。LOOP_MONTAGES 里已标记它要循环。
    ("Boss_Guard_Idle_Montage",  f"{SRC_IP}/Block_Hold_Anim",            0.20, 0.20),
]

# 四向受击的名字（C++ 侧按这些路径兜底加载，名字必须完全一致）
HIT_REACT_NAMES = [
    "Boss_Hit_F_Montage", "Boss_Hit_B_Montage",
    "Boss_Hit_L_Montage", "Boss_Hit_R_Montage",
]


def _enable_root_motion(asset, path, label):
    """把资产的 Enable Root Motion 勾上。

    注意：真正决定"有没有根位移"的是【源动画 AnimSequence】上的这个开关。
    但源动画都在 .../Root-Motion/ 目录下、天生带根位移，所以这里对序列也顺手写一次，
    保证即使之前被谁关掉了也能恢复。
    """
    if asset is None:
        unreal.log_warning(f"[RM] {label} 资产为空，跳过")
        return
    try:
        before = asset.get_editor_property("enable_root_motion")
    except Exception as exc:
        # UAnimMontage 上这个属性在不同版本里可能不存在，属于正常情况
        unreal.log(f"[RM] {label} 读不到 enable_root_motion（{exc}），跳过")
        return
    if before is True:
        return
    try:
        asset.set_editor_property("enable_root_motion", True)
        ASSET_LIB.save_asset(path)
        unreal.log(f"[RM] {label}: enable_root_motion {before} -> True（已保存）")
    except Exception as exc:
        unreal.log_warning(f"[RM] {label} 设置失败：{exc}")


def _tune_blend(montage, name, in_time, out_time):
    """缩短 / 调整蒙太奇的混合时间。

    UE 各版本属性名不一致（新的 FAlphaBlend 结构 blend_in/blend_out，或旧的 blend_in_time/blend_out_time），
    逐个尝试，失败只打日志不影响功能。
    """
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
        except Exception as exc:
            unreal.log(f"[Blend] {name} 的 {prop} 设置失败（不影响功能）：{exc}")


def create_montage(seq_path, montage_name, in_time, out_time, loop=False):
    if not ASSET_LIB.does_directory_exist(OUT_DIR):
        ASSET_LIB.make_directory(OUT_DIR)

    full = f"{OUT_DIR}/{montage_name}"

    seq = ASSET_LIB.load_asset(seq_path)
    if seq is None:
        unreal.log_warning(f"[SKIP] 找不到源动画 {seq_path} → 跳过 {montage_name}")
        return None

    # 源动画也确保勾上根运动（否则蒙太奇勾了也没有根位移曲线可用）
    _enable_root_motion(seq, seq_path, f"源动画 {seq_path.rsplit('/', 1)[-1]}")

    # 强制重建：保证"改了脚本里的取材动画后，重跑就生效"
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

    _enable_root_motion(montage, full, f"蒙太奇 {montage_name}")
    _tune_blend(montage, montage_name, in_time, out_time)

    # 循环蒙太奇（举盾姿态）：让"站着挡"期间一直保持举刀姿势，C++ 侧停盾时靠混合出平滑收回
    if loop:
        try:
            montage.set_editor_property("loop_setting", unreal.MontageLoopSetting.ENABLED)
            ASSET_LIB.save_asset(full)
        except Exception as exc:
            unreal.log_warning(f"[Loop] {montage_name} 设置循环失败（不影响功能）：{exc}")

    ASSET_LIB.save_asset(full)

    length = 0.0
    try:
        length = montage.get_editor_property("sequence_length")
    except Exception:
        pass
    unreal.log(f"[OK] {montage_name}  时长≈{length:.2f}s  源={seq_path.rsplit('/', 1)[-1]}")
    return montage


def retire_old_assets():
    """清理已废弃的蒙太奇（跳砸三件套）。

    招式从「跳砸」换成「突进斩」之后，这三个资产就再也没人加载了。
    留着它们不只是碍眼 —— 更麻烦的是容易在 BP_Boss 里被误拖到招式槽上，
    然后表现为"配了动作却不出招"（因为对应技能已经不存在）。
    """
    removed = []
    for name in RETIRED:
        path = f"{OUT_DIR}/{name}"
        if ASSET_LIB.does_asset_exist(path):
            try:
                ASSET_LIB.delete_asset(path)
                removed.append(name)
            except Exception as exc:
                unreal.log_warning(f"[Retire] 删除 {name} 失败（不影响功能）：{exc}")
    if removed:
        unreal.log_warning(f"[Retire] 已清理废弃蒙太奇 {len(removed)} 个（跳砸 → 突进斩）：{removed}")
    return removed


def verify(created):
    """复核：把 C++ 会去加载的路径逐个检查一遍，缺哪个当场报出来。"""
    unreal.log_warning("========== 复核：C++ 约定路径 ==========")

    expected = [name for name, _, _, _ in MONTAGES]
    missing = []
    for name in expected:
        path = f"{OUT_DIR}/{name}"
        if ASSET_LIB.does_asset_exist(path):
            unreal.log(f"  [存在] {name}")
        else:
            missing.append(name)
            unreal.log_error(f"  [缺失] {name}")

    # 四向受击必须齐全，否则 BOSS 只会播同一段受击
    hit_ok = all(ASSET_LIB.does_asset_exist(f"{OUT_DIR}/{n}") for n in HIT_REACT_NAMES)
    unreal.log_warning(f"四向受击蒙太奇完整：{hit_ok}")

    if missing:
        unreal.log_warning(f"共缺失 {len(missing)} 个：{missing}")
        unreal.log_warning("缺的蒙太奇对应的招式会退化（近战无动作 / 弹幕无施法动作 / 突进斩无姿势），但不会崩。")
    else:
        unreal.log_warning(f"全部 {len(expected)} 个蒙太奇就位，直接 PIE 就能看到 BOSS 出招。")

    unreal.log_warning("=======================================")


def main():
    unreal.log_warning("========== SetupBossMontages 开始 ==========")

    if not ASSET_LIB.does_directory_exist(OUT_DIR):
        ASSET_LIB.make_directory(OUT_DIR)

    retire_old_assets()

    created = []
    for name, seq_path, in_time, out_time in MONTAGES:
        montage = create_montage(seq_path, name, in_time, out_time, name in LOOP_MONTAGES)
        if montage is not None:
            created.append(name)

    unreal.log_warning(f"成功创建 {len(created)}/{len(MONTAGES)} 个蒙太奇")

    verify(created)

    unreal.log_warning(
        "下一步（可选，想在编辑器里调数值才需要）：\n"
        "  1) BP_Boss → Details → Boss|Brain：Phases 留空 = 使用 C++ 内置的默认两阶段\n"
        "     （阶段1：挥砍/弹幕/突进斩；阶段2：同一批招式自动强化 + 大招）\n"
        "  2) 想用自己的数据资产：新建 UBossPhaseData 填进 Phases，内置默认就不再生效\n"
        "  3) 想单独调某一招：BP_Boss → Boss|Brain → DebugForceAttackType\n"
        "     0=近战 1=弹幕 2=突进斩 3=大招，配合 bDrawDebugBrain 看距离带\n"
        "  4) BP_Boss → Boss|Debug → bDrawDebugCombat：画出收尾冲击半径 / 霸体状态\n"
        "  5) 动画混合时间想统一再调一遍：跑 Tools/PolishBossAnimations.py"
    )


main()
