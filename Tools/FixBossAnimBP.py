# -*- coding: utf-8 -*-
"""
FixBossAnimBP.py
================
一键把 BP_Boss 的 Mesh 动画蓝图清空。

为什么需要它
------------
早期 SetupBossKhaimera.py 把 BP_Boss 的 anim_class 指到了
Khaimera_AnimBlueprint —— 那是【玩家角色】的 ABP，内部依赖英雄状态变量，
运行时会除以零 → 整只 BOSS 完全不动 / 永远 T-Pose。

现在 BOSS 动画完全由 C++ 单节点模式驱动（BossCharacterBase::SetupBossAnimation），
不需要任何 AnimBlueprint。本脚本把残留的坏 ABP 引用清掉，避免编辑器实例化它。

用法：编辑器 Output Log 切到 Python，输入本文件绝对路径回车：
      K:/selfgame/demo/Tools/FixBossAnimBP.py
"""

import unreal

ASSET_LIB = unreal.EditorAssetLibrary
BP_BOSS = "/Game/Boss/System/BP_Boss"


def main():
    bp = ASSET_LIB.load_asset(BP_BOSS)
    if bp is None:
        unreal.log_error(f"[FixBossAnimBP] 找不到 {BP_BOSS}")
        return

    try:
        cdo = unreal.get_default_object(bp.generated_class())
    except Exception as exc:
        unreal.log_error(f"[FixBossAnimBP] 取不到 CDO：{exc}")
        return

    try:
        mesh_comp = cdo.get_editor_property("mesh")
    except Exception as exc:
        unreal.log_error(f"[FixBossAnimBP] 取不到 Mesh 组件：{exc}")
        return

    # 读取当前动画蓝图引用（用于日志）
    old = None
    try:
        old = mesh_comp.get_editor_property("anim_class")
    except Exception:
        pass

    try:
        mesh_comp.set_editor_property("anim_class", None)
    except Exception as exc:
        unreal.log_warning(f"[FixBossAnimBP] anim_class 清空失败（C++ 仍会强制单节点模式）：{exc}")
        return

    ASSET_LIB.save_asset(BP_BOSS)
    old_name = old.get_name() if old else "None"
    unreal.log_warning(
        f"[FixBossAnimBP] 已把 BP_Boss 的动画蓝图从 {old_name} 清空。"
        f"BOSS 动画改由 C++ 单节点模式驱动，保存成功：{BP_BOSS}")


main()
