# -*- coding: utf-8 -*-
"""
SetupAttackComboMontages.py
===========================
把 SwordAnimsetPro 的 4 段连击序列（Attack_Combo_01_01 ~ 04）拆成 4 个独立 Montage：
    Attack_Combo_01_Montage / 02 / 03 / 04
然后按 1→2→3→4 顺序填进 GA_NormalAttack 的 ComboMontages 数组。

这样鼠标左键每按一次就续接下一段（共 4 段），松开或中断自动回到第 1 段。
连段窗口 / 命中窗口由 C++ 计时驱动，不依赖蒙太奇里的通知，所以这里不用额外加 Notify。

用法：编辑器 Output Log 右上角切到 Python，输入框填绝对路径回车：
      K:/selfgame/demo/Tools/SetupAttackComboMontages.py
（也可整段粘贴运行，无需重编 C++）
"""

import unreal

ASSET_LIB = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

OUT_DIR = "/Game/FightPlayer/Animation"

# 源序列（AnimSequence），对应第 1~4 段
SOURCE_SEQS = [
    "/Game/SwordAnimsetPro/Animations/Root-Motion/Attack_Combo_01_01_Anim",
    "/Game/SwordAnimsetPro/Animations/Root-Motion/Attack_Combo_01_02_Anim",
    "/Game/SwordAnimsetPro/Animations/Root-Motion/Attack_Combo_01_03_Anim",
    "/Game/SwordAnimsetPro/Animations/Root-Motion/Attack_Combo_01_04_Anim",
]
MONTAGE_NAMES = [
    "Attack_Combo_01_Montage",
    "Attack_Combo_02_Montage",
    "Attack_Combo_03_Montage",
    "Attack_Combo_04_Montage",
]


def _enable_root_motion(asset, path, label):
    """把资产的 Enable Root Motion 勾上。

    这是消除“普攻位移被拉回”的关键：
      - 源动画在 Animations/Root-Motion/ 下，是天生的【带根骨位移】动画；
      - 蒙太奇若没勾 Enable Root Motion，根骨位移就不会被提取去推动胶囊体，
        而是直接推着 Mesh 走，动画一播完（/下一段开始）Mesh 弹回原位 → 表现为“拉回”；
      - 勾上之后位移走标准根运动通道，真实推动角色，不会回弹。
    """
    if asset is None:
        unreal.log_warning(f"[RM] {label} 资产为空，跳过")
        return
    try:
        before = asset.get_editor_property("enable_root_motion")
    except Exception as exc:
        unreal.log_warning(f"[RM] {label} 读不到 enable_root_motion：{exc}")
        return
    try:
        asset.set_editor_property("enable_root_motion", True)
        ASSET_LIB.save_asset(path)
        unreal.log(f"[RM] {label}: enable_root_motion {before} -> True（已保存）")
    except Exception as exc:
        unreal.log_warning(f"[RM] {label} 设置失败：{exc}")


def _tune_blend(montage, name, in_time=0.10, out_time=0.15):
    """缩短蒙太奇的混合（BlendIn / BlendOut）时间 → 连段之间、收招→移动的切换更干脆跟手。

    UE 各版本属性名不一致（新的 FAlphaBlend 结构 `blend_in`/`blend_out`，或旧的
    `blend_in_time`/`blend_out_time`），这里逐个尝试，失败只打日志不影响功能。
    """
    for prop, value in (("blend_in", in_time), ("blend_out", out_time)):
        try:
            blend = montage.get_editor_property(prop)
            blend.set_editor_property("blend_time", value)
            montage.set_editor_property(prop, blend)
            unreal.log(f"[Blend] {name}.{prop}.blend_time = {value}")
            continue
        except Exception:
            pass
        try:
            montage.set_editor_property(f"{prop}_time", value)
            unreal.log(f"[Blend] {name}.{prop}_time = {value}")
        except Exception as exc:
            unreal.log(f"[Blend] {name} 的 {prop} 设置失败（不影响功能）：{exc}")


def create_montage(seq_path, montage_name, out_dir):
    if not ASSET_LIB.does_directory_exist(out_dir):
        ASSET_LIB.make_directory(out_dir)
    full = f"{out_dir}/{montage_name}"

    # 注意：早期 SetupCombatMontages.py 曾用“整段 4 连击”源（Attack_Combo_01_Anim）生成过同名
    # Attack_Combo_01_Montage，会导致“点一下就放四段”。这里【强制重建】，删除已存在的再生成，
    # 保证 4 段蒙太奇全部来自单段源（_01_01~04），不再 SKIP 保留旧的错误资产。
    if ASSET_LIB.does_asset_exist(full):
        unreal.log(f"[DEL] 删除已存在的 {montage_name}（强制重建为单段）")
        ASSET_LIB.delete_asset(full)

    seq = ASSET_LIB.load_asset(seq_path)
    if seq is None:
        unreal.log_warning(f"[SKIP] 找不到序列 {seq_path}")
        return None

    # 源动画也要启用根运动（否则动画没有根运动曲线，蒙太奇勾了也没位移）
    _enable_root_motion(seq, seq_path, f"源动画 {seq_path.rsplit('/', 1)[-1]}")

    factory = unreal.AnimMontageFactory()
    seq_skel = seq.get_editor_property("skeleton")
    factory.set_editor_property("target_skeleton", seq_skel)
    factory.set_editor_property("source_animation", seq)
    montage = TOOLS.create_asset(montage_name, out_dir, unreal.AnimMontage, factory)
    if montage is not None:
        # 关键：启用蒙太奇的根运动，让攻击前冲走根运动通道（真实位移、不回弹）
        _enable_root_motion(montage, full, f"蒙太奇 {montage_name}")
        # 混合时间缩短 → 连段/收招切到移动更利落，不会有拖泥带水的“黏”感
        _tune_blend(montage, montage_name)
        ASSET_LIB.save_asset(full)
        length = 0.0
        try:
            length = montage.get_editor_property("sequence_length")
        except Exception:
            pass
        unreal.log(f"[OK] 创建 Montage: {montage_name}  时长≈{length:.2f}s（应为单段，远小于整段 4 连击）")
    else:
        unreal.log_warning(f"[FAIL] 创建 {montage_name} 失败")
    return montage


def get_cdo(asset_path):
    asset_name = asset_path.rsplit("/", 1)[-1]
    cdo = unreal.load_object(None, f"{asset_path}.Default__{asset_name}_C")
    if cdo is None:
        raise RuntimeError(f"找不到 CDO: {asset_path}.Default__{asset_name}_C")
    return cdo


def main():
    montages = []
    for src, name in zip(SOURCE_SEQS, MONTAGE_NAMES):
        m = create_montage(src, name, OUT_DIR)
        if m is not None:
            montages.append(m)

    if len(montages) != 4:
        unreal.log_error(f"[ABORT] 只成功创建 {len(montages)} 个蒙太奇，需 4 个才能构成 4 段连击")
        return

    cdo = get_cdo("/Game/FightPlayer/Abilities/GA_NormalAttack")
    cdo.set_editor_property("combo_montages", montages)
    ASSET_LIB.save_asset("/Game/FightPlayer/Abilities/GA_NormalAttack")
    unreal.log_warning("GA_NormalAttack.ComboMontages = [01, 02, 03, 04]（顺序即连段顺序）")
    unreal.log_warning("SetupAttackComboMontages 完成。直接 PIE：左键 4 连击，窗口内再按续下一段。")


main()
