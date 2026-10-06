# -*- coding: utf-8 -*-
"""
SetupParryMontages.py
=====================
生成「弹刀(Parry)」的整套动作，并写进 GA_Parry 的对应槽位。

弹刀一共 4 个动作（全部来自 SwordAnimsetPro 的格挡/招架动画，In-Place，无根位移）：

    阶段          Montage 资产                源动画
    ------------  --------------------------  ------------------------------------------------
    起手（抬剑）   Parry_Enter_Montage         In-Place/Block_In_Anim
    持剑架势       Parry_Hold_Montage          In-Place/Block_Hold_Anim
    弹刀成功反击   Parry_Montage               In-Place/Block_CounterAttacks_Anim
    收招（没弹到） Parry_Exit_Montage          In-Place/Block_Out_Anim

对应 C++：UGA_PlayerParry（鼠标右键触发）
    按下右键 → 播 起手 + 开判定窗(0.30s)
       ├ 窗内被打到 → ApplyHit 判定弹反成功 → 播 弹刀成功反击
       └ 窗内没被打到 → 判定窗到点 → 播 收招

用法：编辑器 Output Log 右上角切到 Python，输入框填绝对路径回车：
      K:/selfgame/demo/Tools/SetupParryMontages.py
（也可整段粘贴运行；C++ 侧也有按路径兜底加载，不跑脚本也能玩，但跑了才有完整三段动作）
"""

import unreal

ASSET_LIB = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()

OUT_DIR = "/Game/FightPlayer/Animation"

# (Montage 资产名, 源动画路径)
PARRY_MONTAGES = [
    ("Parry_Enter_Montage", "/Game/SwordAnimsetPro/Animations/In-Place/Block_In_Anim"),
    ("Parry_Hold_Montage",  "/Game/SwordAnimsetPro/Animations/In-Place/Block_Hold_Anim"),
    ("Parry_Montage",       "/Game/SwordAnimsetPro/Animations/In-Place/Block_CounterAttacks_Anim"),
    ("Parry_Exit_Montage",  "/Game/SwordAnimsetPro/Animations/In-Place/Block_Out_Anim"),
]

# GA 属性名(snake_case) ↔ Montage 资产名
PROP_MAP = {
    "parry_enter_montage": "Parry_Enter_Montage",
    "parry_hold_montage":  "Parry_Hold_Montage",
    "parry_montage":       "Parry_Montage",          # 弹刀成功后的反击动作
    "parry_exit_montage":  "Parry_Exit_Montage",
}

GA_PARRY = "/Game/FightPlayer/Abilities/GA_Parry"


def _tune_blend(montage, name, in_time=0.08, out_time=0.12):
    """缩短混合时间：招架→反击、收招→移动的切换更干脆（属性名试两种写法，失败只告警）。"""
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
            unreal.log("[Blend] %s 的 %s 设置失败（不影响功能）：%s" % (name, prop, exc))


def create_montage(seq_path, montage_name, out_dir):
    full = "%s/%s" % (out_dir, montage_name)
    if not ASSET_LIB.does_directory_exist(out_dir):
        ASSET_LIB.make_directory(out_dir)

    # 强制重建：这些资产都是脚本生成的，删掉重建才能保证源动画正确
    # （历史上踩过“旧资产没被覆盖 → 播的还是错误动画”的坑）
    if ASSET_LIB.does_asset_exist(full):
        unreal.log("[DEL] 删除已存在的 %s（强制重建）" % montage_name)
        ASSET_LIB.delete_asset(full)

    seq = ASSET_LIB.load_asset(seq_path)
    if seq is None:
        unreal.log_warning("[SKIP] 找不到源动画 %s" % seq_path)
        return None

    factory = unreal.AnimMontageFactory()
    factory.set_editor_property("target_skeleton", seq.get_editor_property("skeleton"))
    factory.set_editor_property("source_animation", seq)

    montage = TOOLS.create_asset(montage_name, out_dir, unreal.AnimMontage, factory)
    if montage is None:
        unreal.log_warning("[FAIL] 创建 %s 失败（源：%s）" % (montage_name, seq_path))
        return None

    _tune_blend(montage, montage_name)
    ASSET_LIB.save_asset(full)

    length = 0.0
    try:
        length = montage.get_editor_property("sequence_length")
    except Exception:
        pass
    unreal.log("[OK] %s  时长≈%.2fs  ← %s" % (montage_name, length, seq_path.rsplit("/", 1)[-1]))
    return montage


def get_cdo(asset_path):
    asset_name = asset_path.rsplit("/", 1)[-1]
    cdo = unreal.load_object(None, "%s.Default__%s_C" % (asset_path, asset_name))
    if cdo is None:
        raise RuntimeError("找不到 CDO: %s.Default__%s_C" % (asset_path, asset_name))
    return cdo


def main():
    created = {}
    for montage_name, seq_path in PARRY_MONTAGES:
        m = create_montage(seq_path, montage_name, OUT_DIR)
        if m is not None:
            created[montage_name] = m

    if not created:
        unreal.log_error("[ABORT] 一个弹刀动作都没生成成功，请检查源动画路径")
        return

    # 写进 GA_Parry 的 CDO（属性在生成类的 CDO 上，不在 Blueprint 资产对象上）
    cdo = get_cdo(GA_PARRY)
    filled = []
    for prop, montage_name in PROP_MAP.items():
        asset = created.get(montage_name)
        if asset is None:
            unreal.log_warning("[GA] %s 没生成成功，跳过" % montage_name)
            continue
        try:
            cdo.set_editor_property(prop, asset)
            filled.append("%s=%s" % (prop, montage_name))
        except Exception as exc:
            unreal.log_warning("[GA] 写 %s 失败：%s" % (prop, exc))

    ASSET_LIB.save_asset(GA_PARRY)
    unreal.log_warning("[GA] GA_Parry 已写入：%s" % (", ".join(filled) if filled else "（无）"))
    unreal.log_warning("SetupParryMontages 完成。Ctrl+Shift+S 全部保存，然后 PIE：鼠标右键 = 弹刀。")
    unreal.log_warning("  · 抬剑起手 → 窗内没弹到会播“收招”自动结束（0.30s 判定窗）")
    unreal.log_warning("  · 窗内被 Boss 攻击命中 → 播“弹刀反击”动作，Boss 吃大硬直")


main()
