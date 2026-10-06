# -*- coding: utf-8 -*-
"""
把 Content/BGM 里的音频文件导入成 SoundWave 资产，并打开"循环"。

为什么需要脚本：
    音频文件只是磁盘上的 .mp3，UE 不会直接用它 —— 必须先在 Content 下生成
    配套的 .uasset（SoundWave / SoundCue），C++ 的 StaticLoadObject 才能按路径加载。
    手工导入要点：选中 mp3 → 右键 Import → 目标 /Game/BGM。
    这里代劳，顺便做两件手工容易漏的事：
      ① 资产名改成 BGM_BossTheme（原始文件名 70 个字符，写进 C++ 很难看）
      ② 直接把 SoundWave 的 Looping 打开（编辑器里那个勾）

跑法（不用开编辑器）：
    "K:/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "K:/selfgame/demo/demo.uproject" \
        -run=pythonscript -script="K:/selfgame/demo/Tools/ImportBgm.py" \
        -unattended -NoLiveCoding -NoSound -NullRHI
输出：K:/selfgame/demo/Saved/import_bgm.txt
"""
import os

import unreal

SRC_DIR = r"K:/selfgame/demo/Content/BGM"
DEST = "/Game/BGM"
ASSET_NAME = "BGM_BossTheme"
OUT = r"K:/selfgame/demo/Saved/import_bgm.txt"

_lines = []


def emit(s):
    _lines.append(str(s))
    unreal.log(s)


def save():
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(_lines))
    unreal.log("[bgm] 报告写入 %s" % OUT)


def step(label, fn):
    try:
        return fn()
    except Exception as exc:
        emit("[bgm] 步骤失败「%s」：%s: %s" % (label, type(exc).__name__, exc))
        return None


def getp(obj, *names):
    for n in names:
        try:
            return obj.get_editor_property(n)
        except Exception:
            continue
    return None


def main():
    emit("[bgm] ===== 导入 BGM =====")
    if not os.path.isdir(SRC_DIR):
        emit("[bgm] ✗ 源目录不存在：%s" % SRC_DIR)
        save()
        return

    sources = [os.path.join(SRC_DIR, f) for f in sorted(os.listdir(SRC_DIR))
               if f.lower().endswith((".mp3", ".wav", ".ogg", ".flac", ".aiff", ".wma"))]
    emit("[bgm] 源文件：%s" % [os.path.basename(s) for s in sources])
    if not sources:
        emit("[bgm] ✗ 没找到音频文件")
        save()
        return

    src = sources[0]
    dest_path = "%s/%s" % (DEST, ASSET_NAME)
    exists = unreal.EditorAssetLibrary.does_asset_exist(dest_path)

    if exists:
        # 资产已在（上一轮导过的）→ 跳过导入，只补 Looping 勾。
        # 这样脚本可以反复跑（改音量/改勾都靠它），不会每次都重导 3MB 的 mp3。
        emit("[bgm] 资产已存在，跳过导入，只校正属性：%s" % dest_path)
    else:
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", src)
        task.set_editor_property("destination_path", DEST)
        task.set_editor_property("destination_name", ASSET_NAME)
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", True)

        tools = step("取 AssetTools", lambda: unreal.AssetToolsHelpers.get_asset_tools())
        if tools is None:
            save()
            return
        step("执行导入", lambda: tools.import_asset_tasks([task]))
        paths = step("读导入结果", lambda: list(task.get_editor_property("imported_object_paths")) or [])
        emit("[bgm] 导入结果对象：%s" % paths)
        # ⚠ 导入过程日志里会刷 "Decoder for AudioFormat 'BINKA' not found" +
        #   "Ensure condition failed: Factory"（SoundWave.cpp）—— 那是工厂探测过程的噪音，
        #   SoundWave 本身照样生成成功（看下面的资产类/时长），不要据此判断失败。

    sound = step("加载 SoundWave", lambda: unreal.load_asset(dest_path))
    if sound is None:
        emit("[bgm] ✗ 导入后仍加载不到 %s（看上面的错误）" % dest_path)
        save()
        return

    emit("[bgm] 资产类 = %s" % sound.get_class().get_name())
    emit("[bgm] 时长 = %s 秒" % step("读时长", lambda: sound.get_editor_property("duration")))

    # SoundWave 的循环勾（UE5.7 的 USoundWave::bLooping，Python 属性名是 looping）
    # ⚠ 别用 `if name in dir(sound)` 来筛 —— UPROPERTY 不一定出现在 dir() 里
    #   （实测 dir() 只有 get_loop_regions，looping 却能设），必须逐个 try。
    loop_props = [p for p in dir(sound) if "loop" in p.lower()]
    emit("[bgm] dir() 里含 loop 的成员：%s" % loop_props)
    turned_on = False
    for name in ("looping", "b_looping", "bLooping"):
        try:
            sound.set_editor_property(name, True)
            emit("[bgm] ✓ 已把 %s 设为 True（这就是编辑器 Details 里的 Looping 勾）" % name)
            turned_on = True
            break
        except Exception as exc:
            emit("[bgm]   · %s 设不了：%s" % (name, exc))
    if not turned_on:
        emit("[bgm] ⚠ 没设成 → 循环改由 C++ 侧 StartBgm() 在内存里补（见 FightGameMode.cpp）")

    step("标记脏", lambda: sound.modify())
    emit("[bgm] save_loaded_asset = %s" % step(
        "保存", lambda: unreal.EditorAssetLibrary.save_loaded_asset(sound)))

    emit("[bgm] ===== 完成 =====")
    emit("[bgm] C++ 里用这条路径加载：")
    emit("[bgm]   TSoftObjectPtr<USoundBase>(FSoftObjectPath(TEXT(\"/%s.%s\")))" % (dest_path, ASSET_NAME))
    save()


main()
