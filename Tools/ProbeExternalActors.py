# -*- coding: utf-8 -*-
"""
列出 FightGround 的 World Partition 外部 Actor（__ExternalActors__）里到底有什么。

为什么需要它：`EditorLevelLibrary.get_all_level_actors()` 只返回【已加载 cell】的 Actor，
FightGround 是 World Partition 关卡（有个 89km 的 Landscape），所以它只报 10 个 Actor，
BOSS / 地面细节全在磁盘上的外部 Actor 文件里没被加载 —— 会误判成"关卡里没有 BOSS"。

结果写到 Saved/probe_external_actors.txt
"""
import unreal
import glob
import os
import collections

ROOT = r"K:/selfgame/demo/Content/__ExternalActors__/FightGround"
OUT = r"K:/selfgame/demo/Saved/probe_external_actors.txt"

_lines = []


def emit(s):
    _lines.append(str(s))


def main():
    files = glob.glob(os.path.join(ROOT, "**", "*.uasset"), recursive=True)
    emit("[ext] 外部 Actor 文件数 = %d" % len(files))

    counter = collections.Counter()
    samples = {}
    failed = 0

    for f in files:
        pkg_path = f.replace("\\", "/").replace("K:/selfgame/demo/Content/", "/Game/")
        pkg_path = pkg_path[:-len(".uasset")]  # 去掉扩展名
        try:
            obj = unreal.load_asset(pkg_path)
        except Exception:
            failed += 1
            continue
        if obj is None:
            failed += 1
            continue
        try:
            cls = obj.get_class().get_name()
        except Exception:
            failed += 1
            continue
        counter[cls] += 1
        if cls not in samples:
            try:
                samples[cls] = (obj.get_actor_location(), obj.get_actor_label())
            except Exception:
                samples[cls] = (None, "?")

    emit("[ext] 加载失败 %d 个" % failed)
    emit("[ext] ---- 按类型统计 ----")
    for cls, n in counter.most_common():
        loc, label = samples[cls]
        if loc is not None:
            emit("[ext]  %-40s x%-4d  首个 @ (%8.0f, %8.0f, %8.0f)  %s" % (
                cls, n, loc.x, loc.y, loc.z, label))
        else:
            emit("[ext]  %-40s x%-4d" % (cls, n))

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(_lines))


main()
