# -*- coding: utf-8 -*-
# 无头跑一局游戏，只为拿到 Saved/Logs/demo.log 里的 [Boss][anim自检] 行。
# 存在的意义：验证行走动画改动时，【不靠肉眼】也能从日志判定
# "Jog 有没有反复换向 / 播放时间有没有反复归零 / 倍速有没有塌到 0.1x"。
# 用法：python Tools/RunHeadlessGame.py [秒数] [控制台命令] [地图路径]
#   例：python Tools/RunHeadlessGame.py 70 "demo.BossGuardTest 8"
#       → 无头跑 70 秒，并让 BOSS 每 1.2 秒挨一刀（验证格挡判定真的会触发）
#   例：python Tools/RunHeadlessGame.py 40 "" /Game/FightGround
#       → 显式指定关卡（不写就用 Config/DefaultEngine.ini 里的 GameDefaultMap）
import subprocess, time, sys

EDITOR = "K:/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
UPROJECT = "K:/selfgame/demo/demo.uproject"
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 75.0
EXEC = sys.argv[2] if len(sys.argv) > 2 else ""
# ⚠ 2026-10-04：以前这里硬编码 "/Game/FightGround"，结果换了默认关卡（/Game/Fab/FightArena）
#   之后无头跑的仍是旧图 —— 验证会得出"新关卡没问题"的错误结论。
#   现在【默认不传地图】，让引擎走 GameDefaultMap；要指定再从命令行给。
MAP = sys.argv[3] if len(sys.argv) > 3 else ""

cmd = [EDITOR, UPROJECT]
if MAP:
    cmd.append(MAP)
    print("[run] 指定关卡：%s" % MAP, flush=True)
else:
    print("[run] 关卡 = GameDefaultMap（Config/DefaultEngine.ini）", flush=True)
cmd += ["-game", "-NullRHI", "-NoSound", "-NoSplash", "-unattended", "-NoLiveCoding"]
if EXEC:
    cmd.append('-ExecCmds=%s' % EXEC)
    print("[run] 附加控制台命令：%s" % EXEC, flush=True)
print("[run] 启动无头对局 %.0f 秒" % SECONDS, flush=True)
proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print("[run] pid =", proc.pid, flush=True)
time.sleep(SECONDS)
proc.terminate()
try:
    proc.wait(timeout=20)
except Exception:
    proc.kill()
print("[run] 结束，去看 Saved/Logs/demo.log 里的 [Boss][anim自检]", flush=True)
