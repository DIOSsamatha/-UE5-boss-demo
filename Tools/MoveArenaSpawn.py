# -*- coding: utf-8 -*-
"""
把 FightArena 的玩家出生点挪到 BOSS 面前 15m（并让双方互相面对）。

为什么需要：
    实测日志（无头跑）——
        [Boss][心跳] 状态=Idle待机 已停留=15.0s 距离=8555
    关卡里 PlayerStart 在 Y=+3849，BP_Boss 在 Y=-4706，相隔 **85.6m**，
    而 BOSS 大脑的 `AggroRange = 1600`（16m）—— 距离远超仇恨范围，
    BOSS 从头到尾都停在 Idle，连追都不会追，整局打不起来。

    BOSS 的 Z（2565）和 PlayerStart 的 Z（2649）都是用户肉眼放在大理石走道上的，
    只差 84cm，说明这段桥面基本是平的 —— 所以这里【只改 PlayerStart 的 X/Y 和朝向】，
    两个高度都保持用户验证过的原值，风险最小。

跑法（不用开编辑器）：
    "K:/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "K:/selfgame/demo/demo.uproject" \
        -run=pythonscript -script="K:/selfgame/demo/Tools/MoveArenaSpawn.py" \
        -unattended -NoLiveCoding -NoSound -NullRHI
"""
import math

import unreal

LEVEL = "/Game/Fab/FightArena"
OUT = r"K:/selfgame/demo/Saved/move_arena_spawn.txt"

# 玩家出生点与 BOSS 的距离：必须 < AggroRange(1600)，留点余量取 1500
SPAWN_DISTANCE = 1500.0

_lines = []


def emit(s):
    _lines.append(str(s))
    unreal.log(s)


def save():
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(_lines))
    unreal.log("[move] 报告写入 %s" % OUT)


def step(label, fn):
    try:
        return fn()
    except Exception as exc:
        emit("[move] 步骤失败「%s」：%s: %s" % (label, type(exc).__name__, exc))
        return None


def dist(a, b):
    """unreal.Vector 没有 .size()，也不支持直接相减得到长度，老老实实手算。"""
    return math.sqrt((a.x - b.x) ** 2 + (a.y - b.y) ** 2 + (a.z - b.z) ** 2)


def yaw_to(dx, dy):
    """从 (dx,dy) 得到 Unreal 的 yaw（度）。forward = (cos yaw, sin yaw)。"""
    return math.degrees(math.atan2(dy, dx))


def make_rot(yaw_deg):
    """造一个只绕 Z 转的 Rotator。

    ⚠ `unreal.Rotator(a, b, c)` 的位置参数顺序不好记（上一版把 yaw 写进 pitch 槽，
      结果双方朝向全错），所以这里优先用【关键字】构造，失败再退到 MathLibrary。
    """
    try:
        return unreal.Rotator(roll=0.0, pitch=0.0, yaw=float(yaw_deg))
    except Exception as exc:
        emit("[move]   ⚠ 关键字构造 Rotator 失败（%s），改用 MathLibrary" % exc)
    math_lib = getattr(unreal, "MathLibrary", None)
    if math_lib is not None:
        return math_lib.find_look_at_rotation(
            unreal.Vector(0.0, 0.0, 0.0),
            unreal.Vector(math.cos(math.radians(yaw_deg)),
                          math.sin(math.radians(yaw_deg)), 0.0))
    return None


def main():
    emit("[move] ===== 调整 FightArena 出生点 =====")

    step("加载关卡", lambda: unreal.EditorLevelLibrary.load_level(LEVEL))
    actors = step("列 Actor", lambda: unreal.EditorLevelLibrary.get_all_level_actors()) or []

    start = None
    boss = None
    for a in actors:
        try:
            lb = a.get_actor_label()
        except Exception:
            continue
        if lb == "PlayerStart":
            start = a
        elif lb == "BP_Boss":
            boss = a

    if start is None or boss is None:
        emit("[move] ✗ 找不到 PlayerStart 或 BP_Boss（start=%s boss=%s）" % (start, boss))
        save()
        return

    s_loc = start.get_actor_location()
    s_rot = start.get_actor_rotation()
    b_loc = boss.get_actor_location()
    b_rot = boss.get_actor_rotation()
    emit("[move] 改前 PlayerStart loc=(%.0f,%.0f,%.0f) rot=(%.0f,%.0f,%.0f)" % (
        s_loc.x, s_loc.y, s_loc.z, s_rot.roll, s_rot.pitch, s_rot.yaw))
    emit("[move] 改前 BP_Boss    loc=(%.0f,%.0f,%.0f) rot=(%.0f,%.0f,%.0f)  距离=%.0f" % (
        b_loc.x, b_loc.y, b_loc.z, b_rot.roll, b_rot.pitch, b_rot.yaw, dist(s_loc, b_loc)))

    # 方向：从 BOSS 指向【用户原来摆的 PlayerStart】的 XY 方向（保证落在同一段桥面上）
    dx = s_loc.x - b_loc.x
    dy = s_loc.y - b_loc.y
    length = math.sqrt(dx * dx + dy * dy) or 1.0
    nx, ny = dx / length, dy / length

    new_loc = unreal.Vector(
        b_loc.x + nx * SPAWN_DISTANCE,
        b_loc.y + ny * SPAWN_DISTANCE,
        s_loc.z,  # 高度保持用户验证过的原值
    )

    player_yaw = yaw_to(-nx, -ny)   # 玩家朝 BOSS
    boss_yaw = yaw_to(nx, ny)       # BOSS 朝玩家

    step("移动 PlayerStart", lambda: start.set_actor_location(new_loc, False, False))
    r_player = make_rot(player_yaw)
    r_boss = make_rot(boss_yaw)
    emit("[move] 目标朝向：玩家 yaw=%.1f  BOSS yaw=%.1f" % (player_yaw, boss_yaw))
    if r_player is not None:
        step("转 PlayerStart", lambda: start.set_actor_rotation(r_player, False))
    if r_boss is not None:
        step("转 BP_Boss", lambda: boss.set_actor_rotation(r_boss, False))

    s2 = start.get_actor_location()
    s2r = start.get_actor_rotation()
    b2r = boss.get_actor_rotation()
    emit("[move] 改后 PlayerStart loc=(%.0f,%.0f,%.0f) rot(roll,pitch,yaw)=(%.1f,%.1f,%.1f)" % (
        s2.x, s2.y, s2.z, s2r.roll, s2r.pitch, s2r.yaw))
    emit("[move] 改后 BP_Boss     rot(roll,pitch,yaw)=(%.1f,%.1f,%.1f)" % (
        b2r.roll, b2r.pitch, b2r.yaw))
    emit("[move] 新距离 = %.0fcm（仇恨范围 1600cm）" % dist(s2, boss.get_actor_location()))

    ok = step("保存关卡", lambda: unreal.EditorLevelLibrary.save_current_level())
    emit("[move] 保存关卡 = %s" % ok)
    save()


main()
