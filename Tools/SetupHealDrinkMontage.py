# -*- coding: utf-8 -*-
"""
SetupHealDrinkMontage.py
========================
生成回血"酒"的喝酒动作蒙太奇：
    /Game/FightPlayer/Animation/Heal_Drink_Montage

源动画从 SwordAnimsetPro 取"抬手到嘴边"的占位序列（Buff_In_Anim）。
这是一段【原地（In-Place）】动画 —— 故意【不】启用根运动：
喝酒时玩家被 UGA_PlayerUseHeal 锁住 WASD，若再带根位移会"滑步"，
所以蒙太奇保持原地，实际位移完全交给 C++ 的移动锁控制。

C++ 侧读取路径在 Core/DemoCombatTuning.h 的 HealMontagePath，
UGA_PlayerUseHeal::ActivateAbility 里 HealMontage 为空就按该路径 LoadMontage 兜底，
所以只要本脚本生成的资产存在，喝酒动画就会自动接上，无需改蓝图。

注意：喝酒蒙太奇用的是【普通（非加法）】序列。加法动画在单节点动画实例下会被
引擎拒绝设置（刷 "Setting an additive animation ... is not allowed"），播放不出来。
本脚本会自动跳过加法序列、只选普通序列作源，实在找不到就明确报错。

用法（编辑器内，无需重编 C++）：
    Output Log 右上角切到 Python，输入绝对路径回车：
        K:/selfgame/demo/Tools/SetupHealDrinkMontage.py
"""

import unreal

ASSET_LIB = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

OUT_DIR = "/Game/FightPlayer/Animation"
MONTAGE_NAME = "Heal_Drink_Montage"

# 源序列候选（按顺序尝试，取第一个"存在且非加法"的）
SOURCE_CANDIDATES = [
    "/Game/SwordAnimsetPro/Animations/In-Place/Buff_In_Anim",       # 抬手到嘴边：最像"喝酒"
    "/Game/SwordAnimsetPro/Animations/In-Place/Buff_Loop_Anim",     # 退而求其次：循环 buff
    "/Game/SwordAnimsetPro/Animations/In-Place/Idle_Anim",          # 再不行用 idle 兜底
]


def _is_additive(seq):
    """判断 AnimSequence 是不是加法动画（加法不能当蒙太奇源）。"""
    try:
        return seq.get_editor_property("additive_anim_type") != 0
    except Exception:
        # 老版本属性名可能是 is_valid_additive
        try:
            return bool(seq.is_valid_additive())
        except Exception:
            return False


def _pick_source():
    for path in SOURCE_CANDIDATES:
        seq = ASSET_LIB.load_asset(path)
        if seq is None:
            unreal.log_warning(f"[Heal] 候选源不存在，跳过：{path}")
            continue
        if _is_additive(seq):
            unreal.log_warning(f"[Heal] 候选源是加法动画，跳过（单节点实例下无法播放）：{path}")
            continue
        return path, seq
    return None, None


def _tune_blend(montage, name, in_time=0.12, out_time=0.20):
    """缩短混合时间，让喝酒起手/收招切回移动更干脆（与攻击蒙太奇同思路）。"""
    for prop, value in (("blend_in", in_time), ("blend_out", out_time)):
        try:
            blend = montage.get_editor_property(prop)
            blend.set_editor_property("blend_time", value)
            montage.set_editor_property(prop, blend)
            unreal.log(f"[Heal][Blend] {name}.{prop}.blend_time = {value}")
            continue
        except Exception:
            pass
        try:
            montage.set_editor_property(f"{prop}_time", value)
            unreal.log(f"[Heal][Blend] {name}.{prop}_time = {value}")
        except Exception as exc:
            unreal.log(f"[Heal][Blend] {name} 的 {prop} 设置失败（不影响功能）：{exc}")


def create_heal_montage():
    src_path, seq = _pick_source()
    if seq is None:
        unreal.log_error(
            "[Heal] 找不到可用的喝酒源动画（SwordAnimsetPro 的 Buff_In_Anim 等都不存在或非加法）。\n"
            "       请在 SwordAnimsetPro/Animations 确认资源已挂载；或改本脚本的 SOURCE_CANDIDATES。\n"
            "       注意：即使不生成蒙太奇，UGA_PlayerUseHeal 仍会回血（只是没有喝酒动画，会打日志提示）。"
        )
        return None

    if not ASSET_LIB.does_directory_exist(OUT_DIR):
        ASSET_LIB.make_directory(OUT_DIR)
    full = f"{OUT_DIR}/{MONTAGE_NAME}"

    # 强制重建：删掉旧资产再生成，避免残留旧版本。
    if ASSET_LIB.does_asset_exist(full):
        unreal.log(f"[Heal][DEL] 删除已存在的 {MONTAGE_NAME}（强制重建）")
        ASSET_LIB.delete_asset(full)

    factory = unreal.AnimMontageFactory()
    seq_skel = seq.get_editor_property("skeleton")
    factory.set_editor_property("target_skeleton", seq_skel)
    factory.set_editor_property("source_animation", seq)
    montage = TOOLS.create_asset(MONTAGE_NAME, OUT_DIR, unreal.AnimMontage, factory)
    if montage is None:
        unreal.log_error(f"[Heal][FAIL] 创建 {MONTAGE_NAME} 失败")
        return None

    # ⚠ 喝酒【不】启用根运动：玩家原地喝，位移交给 C++ 移动锁，避免滑步。
    # （攻击蒙太奇才需要根运动；喝酒刻意相反，这里不要调 _enable_root_motion。）

    _tune_blend(montage, MONTAGE_NAME)
    ASSET_LIB.save_asset(full)

    length = 0.0
    try:
        length = montage.get_editor_property("sequence_length")
    except Exception:
        pass
    unreal.log(
        f"[Heal][OK] 创建喝酒蒙太奇: {MONTAGE_NAME}  源={src_path.rsplit('/', 1)[-1]}  "
        f"时长≈{length:.2f}s（无根运动，原地播放）"
    )
    return montage


def main():
    m = create_heal_montage()
    if m is None:
        unreal.log_error("[Heal] SetupHealDrinkMontage 未完成（见上方错误）。回血逻辑不受影响，只是没有喝酒动画。")
        return
    unreal.log_warning(
        "[Heal] SetupHealDrinkMontage 完成。Heal_Drink_Montage 已生成于 /Game/FightPlayer/Animation/。\n"
        "       PIE 后按 R 喝酒：播放喝酒动作 + 回 30% 最大生命（整屏绿光 + '+N 生命' 飘字）。"
    )


main()
