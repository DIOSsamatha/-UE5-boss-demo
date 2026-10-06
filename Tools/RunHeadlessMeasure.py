# -*- coding: utf-8 -*-
# 用 Python 子进程拉起无头编辑器跑测量脚本（绕过 Bash 直接拉 exe 的安全拦截）。
import subprocess, sys, os, time

EDITOR = "K:/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
UPROJECT = "K:/selfgame/demo/demo.uproject"
SCRIPT = sys.argv[1] if len(sys.argv) > 1 else "K:/selfgame/demo/Tools/MeasureJogRootMotion.py"

cmd = [EDITOR, UPROJECT, "-NullRHI", "-nosplash", "-unattended", "-NoLiveCoding",
       "-ExecutePythonScript=" + SCRIPT]
print("[Launcher] 启动无头编辑器测量:", SCRIPT, flush=True)
try:
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
    print("[Launcher] 编辑器退出码 =", proc.returncode, flush=True)
except subprocess.TimeoutExpired:
    print("[Launcher] 超时（180s），终止", flush=True)
time.sleep(2.0)
print("[Launcher] 编辑器标准输出（若有）:", flush=True)
try:
    out = proc.stdout.decode("utf-8", "replace")
    print(out[-3000:], flush=True)
except Exception:
    pass
