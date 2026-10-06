# -*- coding: utf-8 -*-
"""列出 Khaimera 所有【真正非加法】的 AnimSequence（用引擎自己的判据，不是猜字符串）。

背景：单节点模式会拒绝加法动画，被拒绝的蒙太奇画面上就是定格/怪异。
之前用"文件里有没有 AdditiveAnimType 字符串"去猜，结果把 E_Ability_Start 之类误判成非加法，
害得突进斩的冲刺段和新增的 Throw_01 都播不出来。这里改用引擎权威判据。

用法（编辑器关闭时，命令行）：
  UnrealEditor-Cmd.exe <proj> -run=pythonscript -script=Tools/_scan_nonadditive.py -unattended -NullRHI
"""
import unreal

SRC = "/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations"
lib = unreal.EditorAssetLibrary

ok, add, other = [], [], []
# UE 5.7 的 list_assets 第一个参数是 DirectoryPath（字符串），不是数组 —— 传数组会抛
# "Cannot nativize 'list' as 'DirectoryPath'"。
for path in lib.list_assets(SRC, recursive=True, include_folder=False):
    asset = lib.load_asset(path)
    if asset is None:
        continue
    # 只关心序列（蒙太奇是序列包出来的，判据同源）
    if not isinstance(asset, unreal.AnimSequence):
        other.append(path.rsplit("/", 1)[-1])
        continue
    try:
        additive = asset.get_editor_property("additive_anim_type")
    except Exception as exc:
        unreal.log_warning("读不到 additive_anim_type: %s (%s)" % (path, exc))
        continue
    name = path.rsplit("/", 1)[-1]
    if additive == unreal.AdditiveAnimationType.AAT_NONE:
        ok.append(name)
    else:
        add.append("%s  [%s]" % (name, additive))

unreal.log_warning("======== 非加法(可用) %d 个 ========" % len(ok))
for n in sorted(ok):
    unreal.log_warning("  " + n)
unreal.log_warning("======== 加法(单节点不可用) %d 个 ========" % len(add))
for n in sorted(add):
    unreal.log_warning("  " + n)
