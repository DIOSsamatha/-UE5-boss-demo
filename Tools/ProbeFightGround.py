# -*- coding: utf-8 -*-
"""
探测现有战斗关卡里到底放了什么 Actor（关卡只有 12KB，靠猜容易漏东西）。
结果写到 Saved/probe_fightground.txt（commandlet 的 stdout 经常被管道吞掉，别指望 print）。

跑法：
    "K:/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "K:/selfgame/demo/demo.uproject" \
        -run=pythonscript -script="K:/selfgame/demo/Tools/ProbeFightGround.py" -unattended -NoLiveCoding -NoSound -NullRHI
"""
import unreal

LEVEL = "/Game/FightGround"
OUT = r"K:/selfgame/demo/Saved/probe_fightground.txt"

_lines = []


def emit(s):
    _lines.append(str(s))
    unreal.log(s)


def main():
    emit("[probe] 加载关卡 %s" % LEVEL)
    if not unreal.EditorLevelLibrary.load_level(LEVEL):
        emit("[probe] 加载失败：%s" % LEVEL)
        _flush()
        return

    actors = unreal.EditorLevelLibrary.get_all_level_actors()
    emit("[probe] 关卡 %s 共 %d 个 Actor" % (LEVEL, len(actors)))
    for a in actors:
        try:
            cls = a.get_class().get_name()
            loc = a.get_actor_location()
            emit("[probe]  %-38s @ (%8.0f, %8.0f, %8.0f)  %s" % (
                cls, loc.x, loc.y, loc.z, a.get_actor_label()))
        except Exception as exc:
            emit("[probe] 读 Actor 失败：%s" % exc)

    # 顺带看看 Fab 那个竞技场网格的信息（尺寸很关键：决定要不要缩放）
    fab = "/Game/Fab/Boss_Fight_Arena_PVP_Arena/boss_fight_arenapvp_arena/StaticMeshes/boss_fight_arenapvp_arena"
    sm = unreal.load_asset(fab)
    if sm:
        bb = sm.get_bounds() if hasattr(sm, "get_bounds") else None
        emit("[probe] Fab 竞技场网格: %s" % sm.get_name())
        try:
            import unreal as u
            b = sm.get_bounds()
            emit("[probe]   包围盒 origin=(%.0f, %.0f, %.0f) extent=(%.0f, %.0f, %.0f)" % (
                b.origin.x, b.origin.y, b.origin.z, b.box_extent.x, b.box_extent.y, b.box_extent.z))
        except Exception as exc:
            emit("[probe]   取包围盒失败: %s" % exc)
    else:
        emit("[probe] Fab 网格加载失败：%s" % fab)

    _flush()


def _flush():
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(_lines))


main()
