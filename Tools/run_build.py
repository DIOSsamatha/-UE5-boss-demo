"""项目构建入口。

默认流程：UBT(Build.bat) -> 失败时自动判定是否为"UBT 启动 cl.exe 静默秒退"故障，
是则自动回退到 Tools/ManualBuild.py（直接用 cl.exe/link.exe 执行 UBT 生成的 .rsp）。

用法：
  python Tools/run_build.py             # 自动（UBT，必要时回退手工）
  python Tools/run_build.py --manual    # 直接走手工编译+链接
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)

ENGINE = r"K:\UE_5.7\Engine\Build\BatchFiles\Build.bat"
PROJECT = os.path.join(PROJ, "demo.uproject")
LOG = os.path.join(PROJ, "build_log.txt")
MANUAL = os.path.join(HERE, "ManualBuild.py")
PYTHON = sys.executable

# 真实编译器诊断的样子，例如 "error C2065"、"fatal error C1083"、"error LNK2019"
REAL_DIAG = re.compile(r"(fatal error|error) [A-Z]{1,4}\d{3,}", re.IGNORECASE)
# UBT 在子进程被"掐断"时的固定措辞
SILENT_FAIL = re.compile(r"Exited with error code (\d+)")


def run_ubt():
    cmd = 'cmd /c "{} demoEditor Win64 Development -Project={} -WaitMutex -FromMsBuild -NoUBA"'.format(
        ENGINE, PROJECT)
    print("[Build] UBT 构建:", cmd, flush=True)
    with open(LOG, "w", encoding="utf-8") as f:
        f.write("========== Build started ==========\n")
        f.flush()
        proc = subprocess.run(cmd, shell=True, stdout=f, stderr=subprocess.STDOUT, text=True)
        f.write("\n========== Build exit code: {} ==========\n".format(proc.returncode))
    print("[Build] UBT 结束，exit code =", proc.returncode, flush=True)
    return proc.returncode


def looks_like_silent_child_failure():
    """判定：UBT 报告多个子进程退出码错误，却没有任何真正的编译诊断。"""
    try:
        with open(LOG, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return False
    n_silent = len(SILENT_FAIL.findall(text))
    n_diag = len(REAL_DIAG.findall(text))
    print("[Build] 子进程退出码行 {} 条，真实编译诊断 {} 条".format(n_silent, n_diag), flush=True)
    # UBT 只有在子进程异常退出时才会打印 "Exited with error code"；
    # 只要没有真正的编译器诊断，就认为子进程是被环境掐断的。
    return n_silent >= 1 and n_diag == 0


def run_manual():
    print("[Build] 改走手工编译+链接（Tools/ManualBuild.py）", flush=True)
    return subprocess.run([PYTHON, MANUAL], cwd=PROJ).returncode


def main():
    if "--manual" in sys.argv:
        sys.exit(0 if run_manual() == 0 else 1)

    rc = run_ubt()
    if rc == 0:
        sys.exit(0)

    if looks_like_silent_child_failure():
        print("[Build] 判定：UBT 能启动 cl.exe 但进程被秒退且无任何诊断 —— 属于环境级故障", flush=True)
        print("[Build] 不是代码错误，自动回退手工构建", flush=True)
        sys.exit(0 if run_manual() == 0 else 1)

    print("[Build] 存在真实编译诊断，请查看 build_log.txt", flush=True)
    sys.exit(rc)


if __name__ == "__main__":
    main()
