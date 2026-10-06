# -*- coding: utf-8 -*-
"""
SetupBossWalkMontages.py
=======================
给 BOSS 生成【行走蒙太奇】——从 Paragon Khaimera 自带的 Jog 序列直接生成。

为什么需要（2026-10-02，第十六轮定案）
--------------------------------------
之前行走动画用的是"单节点 + 直接播序列"（`PlayAnimation(Seq, true)`）。
这条路在单节点模式下有两个先天缺陷，导致玩家看到"BOSS 没有移动动作、整体在地面平移"：
  1. 单节点实例**只在 SetAnimationAsset 的是【蒙太奇】时才会注册该蒙太奇的 Slot 节点**。
     序列没有 Slot → DefaultSlot 永远不存在 → 攻击/受击蒙太奇找不到 Slot → 静默失败。
  2. 行走用序列、招式用蒙太奇，两者**不在同一个 Slot 通道**里，
     招式一来就把行走整个盖掉，行走腿完全停摆；招式一结束又从第 0 帧重播 → 步态永远走不完。

改成【行走也用蒙太奇】之后：
  · 行走与招式都挂在 `DefaultSlot`，走同一条分层通道，切换是引擎标准的 Slot 混合；
  · 蒙太奇资产自带 Slot 定义，`SetAnimationAsset` 时会被注册，Slot 一定存在；
  · 可以显式设置 `Loop = true` 让步态真正循环，不会播一遍就停在末帧。

生成的资产（放在 /Game/Boss/Animation/）：
    Boss_Walk_Fwd_Montage   <- Jog_Fwd     向前
    Boss_Walk_Bwd_Montage   <- Jog_Bwd     向后
    Boss_Walk_Left_Montage  <- Jog_Left    向左
    Boss_Walk_Right_Montage <- Jog_Right   向右
    Boss_Idle_Montage       <- Idle_NonAdditive  待机

用法（编辑器 Output Log → Python 框，输入绝对路径回车）：
    K:/selfgame/demo/Tools/SetupBossWalkMontages.py
"""
import unreal

ASSET_LIB = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

OUT_DIR = "/Game/Boss/Animation"
SRC_ANIM = "/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations"
SLOT = "DefaultSlot"

# (资产名, 源序列文件名, 混合入, 混合出, 是否循环)
#
# ⚠⚠⚠ 2026-10-03 修正：Left/Right 必须用【真横移】的 Strafe_Left / Strafe_Right，
#   不能用 Jog_Left / Jog_Right。
#   Jog_Left / Jog_Right 是"侧着身子往前跑"（脚步仍朝前、身体侧对），
#   适合"边跑边侧看"，**不适合"横向移动"**。
#   BOSS 绕圈走位是真正的横移（切线方向垂直于视线），
#   播 Jog_Left 就会变成"侧身往前跑" + 身体在横着滑 = 溜冰感更重。
#   Strafe_Left / Strafe_Right 才是双脚侧向迈步的真横移。
#
# 目录里还提供了 Jog_*_Start / Jog_*_Stop / Jog_Fwd_CircleLeft 等，
# 暂未使用（先保证主循环正确，衔接类动画可以后续加）。
WALK = [
    ("Boss_Idle_Montage",       "Idle_NonAdditive", 0.20, 0.20, True),
    ("Boss_Walk_Fwd_Montage",   "Jog_Fwd",          0.15, 0.15, True),
    ("Boss_Walk_Bwd_Montage",   "Jog_Bwd",          0.15, 0.15, True),
    ("Boss_Walk_Left_Montage",  "Strafe_Left",      0.15, 0.15, True),
    ("Boss_Walk_Right_Montage", "Strafe_Right",     0.15, 0.15, True),
]


def tune_blend(m, in_t, out_t):
    for prop, val in (("blend_in", in_t), ("blend_out", out_t)):
        try:
            b = m.get_editor_property(prop)
            b.set_editor_property("blend_time", val)
            m.set_editor_property(prop, b)
        except Exception:
            try:
                m.set_editor_property(f"{prop}_time", val)
            except Exception:
                pass


def set_loop(m, loop):
    """把蒙太奇设成循环播。

    UE 5 里 UAnimMontage 的循环由 Slot 节点上的 `LoopAnim` / 或 `AnimMontage` 资产属性控制，
    各版本属性名不一致，逐个尝试；失败只打日志（不循环也不会崩，只是播一遍停住）。
    """
    for prop in ("loop", "loop_anim", "enable_loop"):
        try:
            m.set_editor_property(prop, loop)
            return True
        except Exception:
            continue
    # 退路：逐个 Slot 节点设
    try:
        slots = m.get_editor_property("slot_anim_tracks")
        for st in slots:
            for p in ("loop_anim", "loop"):
                try:
                    st.set_editor_property(p, loop)
                except Exception:
                    pass
    except Exception:
        pass
    return False


def make(seq_file, name, in_t, out_t, loop):
    seq_path = f"{SRC_ANIM}/{seq_file}.{seq_file}"
    seq = ASSET_LIB.load_asset(seq_path)
    if seq is None:
        unreal.log_warning(f"[SKIP] 找不到源动画 {seq_file}")
        return False
    if seq.get_editor_property("enable_root_motion"):
        pass  # in-place 序列不该有根位移，若有则提示
    else:
        unreal.log(f"[i] {seq_file} 根位移=0（原地动画，符合预期）")

    if not ASSET_LIB.does_directory_exist(OUT_DIR):
        ASSET_LIB.make_directory(OUT_DIR)
    full = f"{OUT_DIR}/{name}"
    if ASSET_LIB.does_asset_exist(full):
        ASSET_LIB.delete_asset(full)

    try:
        f = unreal.AnimMontageFactory()
        f.set_editor_property("target_skeleton", seq.get_editor_property("skeleton"))
        f.set_editor_property("source_animation", seq)
        m = TOOLS.create_asset(name, OUT_DIR, unreal.AnimMontage, f)
    except Exception as exc:
        unreal.log_error(f"[FAIL] {name} 创建失败：{exc}")
        return False
    if m is None:
        unreal.log_error(f"[FAIL] {name} create_asset 返回空")
        return False

    # 关键：把 Slot 轨道指向 DefaultSlot，否则这个蒙太奇播起来没有任何效果
    try:
        st = unreal.AnimSlotAnimationTrack()
        st.set_editor_property("slot_name", SLOT)
        st.set_editor_property("source_anim", seq)
        m.get_editor_property("slot_anim_tracks").clear()
        m.get_editor_property("slot_anim_tracks").append(st)
    except Exception as exc:
        unreal.log_warning(f"[Slot] {name} 设置 slot 轨道失败：{exc}（可能属性名不同，需手工检查）")

    tune_blend(m, in_t, out_t)
    ok = set_loop(m, loop)
    ASSET_LIB.save_asset(full)
    unreal.log(f"[OK] {name}  <- {seq_file}  Slot={SLOT} 循环={ok}")
    return True


if not ASSET_LIB.does_directory_exist(OUT_DIR):
    ASSET_LIB.make_directory(OUT_DIR)

cnt = 0
for name, seqf, it, ot, lp in WALK:
    if make(seqf, name, it, ot, lp):
        cnt += 1

unreal.log(f"==== 行走蒙太奇生成完成：{cnt}/{len(WALK)} ====")
