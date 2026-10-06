# -*- coding: utf-8 -*-
"""
加载 /Game/Fab/FightArena，列出关卡里真实存在的 Actor。
用途：验证 SetupArenaLevel.py 放的 PlayerStart / BP_Boss 有没有真的写进 .umap
     （保存前在内存里看到 ≠ 存进文件了）。
结果写到 Saved/probe_arena.txt
"""
import unreal

LEVEL = "/Game/Fab/FightArena"
OUT = r"K:/selfgame/demo/Saved/probe_arena.txt"

_lines = []


def emit(s):
    _lines.append(str(s))


def main():
    emit("[pa] 加载 %s" % LEVEL)
    ok = unreal.EditorLevelLibrary.load_level(LEVEL)
    emit("[pa] load_level = %s，当前世界 = %s" % (
        ok, unreal.EditorLevelLibrary.get_editor_world().get_name()))

    actors = unreal.EditorLevelLibrary.get_all_level_actors()
    emit("[pa] 共 %d 个 Actor" % len(actors))
    starts = []
    for a in actors:
        try:
            cls = a.get_class().get_name()
            loc = a.get_actor_location()
            extra = ""
            if "PlayerStart" in cls:
                enabled = None
                for p in ("b_enabled", "enabled", "player_start_id"):
                    try:
                        enabled = "%s=%s" % (p, a.get_editor_property(p))
                        break
                    except Exception:
                        pass
                extra = "  <-- PlayerStart %s" % (enabled or "?")
                starts.append(a)
            emit("[pa]  %-32s @ (%9.1f, %9.1f, %9.1f)%s" % (
                cls, loc.x, loc.y, loc.z, extra))
        except Exception as exc:
            emit("[pa]   读 Actor 失败：%s" % exc)

    emit("[pa] PlayerStart 数量 = %d" % len(starts))
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(_lines))


main()
