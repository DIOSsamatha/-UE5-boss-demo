# -*- coding: utf-8 -*-
"""
FixLocomotionRootMotion.py
==========================
一键把 Root-Motion 目录下的 Walk/Run 动画强制锁成"原地(in-place)"，
消除混合空间里 root motion 导致的"跑完拉回 / 运动不自然"问题。

用法：
  A. 编辑器内：Output Log 右下角切到 "Python"，输入框填绝对路径：
        K:/selfgame/demo/Tools/FixLocomotionRootMotion.py
  B. 或把本文件内容整段粘贴到 Python 输入框运行。

原理：
  bForceRootLock=True + RootMotionRootLock=Zero 时，引擎把根骨钉死在
  FTransform::Identity，根骨位移被强制归零 → 循环无缝不再拉回，
  位移改由输入速度驱动（8 向横移正需要这个）。

注意：只影响名称含 Walk/Run 的动画；攻击等有意的 root motion Montage 不动。
"""

import unreal

FOLDER = "/Game/SwordAnimsetPro/Animations/Root-Motion"
KEYWORDS = ("Walk", "Run")
LOCK = unreal.RootMotionRootLock.ZERO


def main():
    # list_assets 直接返回 object path 字符串，不依赖 AssetData 的属性访问
    paths = unreal.EditorAssetLibrary.list_assets(
        FOLDER, recursive=False, include_folder=False
    )
    fixed = 0
    for obj_path in paths:
        obj_path = str(obj_path)
        name = obj_path.rsplit(".", 1)[-1].rsplit("/", 1)[-1]
        if not any(k in name for k in KEYWORDS):
            continue
        a = unreal.EditorAssetLibrary.load_asset(obj_path)
        if a is None:
            unreal.log_warning(f"[SKIP] 无法加载 {name}")
            continue
        # 只处理 AnimSequence，跳过 Montage / BlendSpace 等
        if not isinstance(a, unreal.AnimSequence):
            continue
        a.set_editor_property("root_motion_root_lock", LOCK)
        a.set_editor_property("force_root_lock", True)
        unreal.EditorAssetLibrary.save_asset(obj_path)
        fixed += 1
        unreal.log(f"[OK] {name} -> RootMotionRootLock=Zero, ForceRootLock=true")
    unreal.log_warning(f"FixLocomotionRootMotion 完成，共处理 {fixed} 个 locomotion 动画")


main()
