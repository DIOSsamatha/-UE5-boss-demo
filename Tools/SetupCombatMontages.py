# -*- coding: utf-8 -*-
"""
SetupCombatMontages.py
======================
在编辑器里一键完成两件事：
  1) 用 SwordAnimsetPro 的 4 个 Dodge 序列创建 4 个 Montage（前/后/左/右），
     并填进 GA_Dodge 的 DirectionalMontages（顺序：前、后、左、右）。
  2) 把 SwordAnimsetPro 自带的 4 连击 Montage（Attack_Combo_01_Anim）
     填进 GA_NormalAttack 的 ComboMontages[0]。

用法：编辑器 Output Log 切到 Python，输入框填绝对路径回车：
      K:/selfgame/demo/Tools/SetupCombatMontages.py
（也可把本文件内容整段粘贴运行）

说明：
  - 普通攻击的"段数"由 GA_NormalAttack 的 ComboMontages 数组长度决定。
    这里填的是 Attack_Combo_01_Anim（自带 4 连挥砍的整段 Montage），
    即一次出招里有 4 个挥击动作；若要"按键续接下一式"，
    需把 4 段拆成 4 个独立 Montage 分别填入数组（见脚本末尾注释）。
"""

import unreal

ASSET_LIB = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

# 源动画（AnimSequence）
DODGE_SEQS = {
    "F": "/Game/SwordAnimsetPro/Animations/Root-Motion/Dodge_F_Anim",
    "B": "/Game/SwordAnimsetPro/Animations/Root-Motion/Dodge_B_Anim",
    "L": "/Game/SwordAnimsetPro/Animations/Root-Motion/Dodge_L_Anim",
    "R": "/Game/SwordAnimsetPro/Animations/Root-Motion/Dodge_R_Anim",
}
DODGE_MONTAGE_DIR = "/Game/FightPlayer/Animation"
ATTACK_MONTAGE = "/Game/SwordAnimsetPro/Demo/AnimMontage/Attack_Combo_01_Anim"


def create_montage(seq_path, montage_name, out_dir):
    if not ASSET_LIB.does_directory_exist(out_dir):
        ASSET_LIB.make_directory(out_dir)
    if ASSET_LIB.does_asset_exist(f"{out_dir}/{montage_name}"):
        return ASSET_LIB.load_asset(f"{out_dir}/{montage_name}")
    seq = ASSET_LIB.load_asset(seq_path)
    if seq is None:
        unreal.log_warning(f"[SKIP] 找不到序列 {seq_path}")
        return None
    factory = unreal.AnimMontageFactory()
    # UE5.7 的 AnimMontageFactory 属性（见 AnimMontageFactory.h）：
    #   TargetSkeleton  -> python 名 target_skeleton
    #   SourceAnimation -> python 名 source_animation（从 AnimSequence 创建 Montage 用）
    seq_skel = seq.get_editor_property("skeleton")
    factory.set_editor_property("target_skeleton", seq_skel)
    factory.set_editor_property("source_animation", seq)
    montage = TOOLS.create_asset(montage_name, out_dir, unreal.AnimMontage, factory)
    if montage is not None:
        ASSET_LIB.save_asset(f"{out_dir}/{montage_name}")
        unreal.log(f"[OK] 创建 Montage: {montage_name}")
    return montage


def get_cdo(asset_path):
    # 直接按确定性路径加载 CDO："<包路径>.Default__<资产名>_C"
    # （CDO 对象名固定为 Default__<类名>，已从 .uasset 内部字符串核实）
    asset_name = asset_path.rsplit("/", 1)[-1]
    cdo = unreal.load_object(None, f"{asset_path}.Default__{asset_name}_C")
    if cdo is None:
        raise RuntimeError(f"找不到 CDO: {asset_path}.Default__{asset_name}_C")
    return cdo


def main():
    # ---- 1. 闪避：4 向 Montage ----
    dodge_montages = []
    for key in ("F", "B", "L", "R"):
        m = create_montage(DODGE_SEQS[key], f"Dodge_{key}_Montage", DODGE_MONTAGE_DIR)
        if m is not None:
            dodge_montages.append(m)

    if dodge_montages:
        cdo = get_cdo("/Game/FightPlayer/Abilities/GA_Dodge")
        cdo.set_editor_property("directional_montages", dodge_montages)
        ASSET_LIB.save_asset("/Game/FightPlayer/Abilities/GA_Dodge")
        unreal.log_warning(f"GA_Dodge.DirectionalMontages 已填 {len(dodge_montages)} 个（前/后/左/右）")

    # ---- 2. 普通攻击：4 连击 Montage ----
    # 注意：Attack_Combo_01_Anim 虽在 "AnimMontage" 文件夹里，但资产本体是 AnimSequence
    # （运行时类型转换报错证实），而 ComboMontages 数组只收 AnimMontage → 需包一层 Montage
    atk = create_montage(
        "/Game/SwordAnimsetPro/Demo/AnimMontage/Attack_Combo_01_Anim",
        "Attack_Combo_01_Montage",
        DODGE_MONTAGE_DIR,
    )
    if atk is not None:
        cdo = get_cdo("/Game/FightPlayer/Abilities/GA_NormalAttack")
        cdo.set_editor_property("combo_montages", [atk])
        ASSET_LIB.save_asset("/Game/FightPlayer/Abilities/GA_NormalAttack")
        unreal.log_warning("GA_NormalAttack.ComboMontages[0] = Attack_Combo_01_Montage（4 连击）")
    else:
        unreal.log_warning("[SKIP] 攻击序列转换 Montage 失败")

    unreal.log_warning("SetupCombatMontages 完成。无需重编 C++，直接 PIE 测试。")


main()
