"""手工构建 UnrealEditor-demo.dll —— 绕过 UnrealBuildTool 的进程启动故障。

背景：本机环境下 UBT 启动的子进程 cl.exe 会"秒退且无输出"（exit 1），
但同一个 .obj.rsp 由脚本直接用 cl.exe 执行完全正常（改由 UBT 执行则 100% 失败）。
因此本脚本直接接管 UBT 已经生成好的响应文件：
  1) cl.exe 执行 *.obj.rsp  ->  .obj      （按 dep.json 做增量，只编过期的）
  2) link.exe 执行 UnrealEditor-demo.dll.rsp -> Binaries/Win64/UnrealEditor-demo.dll

注意：rsp 中的引擎库是相对路径（../Intermediate/...、../Plugins/...），
链接时工作目录必须是 K:\\UE_5.7\\Engine\\Source，否则 LNK1181。

用法：
  python Tools/ManualBuild.py              # 增量编译 + 链接
  python Tools/ManualBuild.py --force      # 强制全量重编
  python Tools/ManualBuild.py --compile    # 只编译
  python Tools/ManualBuild.py --link       # 只链接
  python Tools/ManualBuild.py --jobs 3     # 并行编译数（默认 2，PCH 约 2.4GB）
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

MSVC_BIN = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64"
CL = os.path.join(MSVC_BIN, "cl.exe")
LINK = os.path.join(MSVC_BIN, "link.exe")

ENGINE_SRC = r"K:\UE_5.7\Engine\Source"
PROJ = r"K:\selfgame\demo"
DOTNET = r"K:\UE_5.7\Engine\Binaries\ThirdParty\DotNet\8.0.412\win-x64\dotnet.exe"
UBT_DLL = r"K:\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.dll"
INTER = os.path.join(PROJ, r"Intermediate\Build\Win64\x64\UnrealEditor\Development\demo")
DLL_RSP = os.path.join(INTER, "UnrealEditor-demo.dll.rsp")
LIB_RSP = os.path.join(INTER, "UnrealEditor-demo.lib.rsp")
LIB_OUT = os.path.join(INTER, "UnrealEditor-demo.lib")
DLL_OUT = os.path.join(PROJ, r"Binaries\Win64\UnrealEditor-demo.dll")
LOG = os.path.join(PROJ, "manual_build_log.txt")
TARGET_META = os.path.join(PROJ, r"Intermediate\Build\Win64\x64\demoEditor\Development\TargetMetadata.dat")
TARGET_FILE = os.path.join(PROJ, r"Binaries\Win64\demoEditor.target")

_logf = None


def log(msg):
    print(msg, flush=True)
    if _logf:
        _logf.write(msg + "\n")
        _logf.flush()


def _mtime(path):
    try:
        return os.path.getmtime(path)
    except OSError:
        return None


def outdated(rsp):
    """返回 (是否需要编译, 原因)"""
    obj = rsp[:-4]                      # 去掉 .rsp
    dep = obj[:-4] + ".dep.json"        # X.cpp.obj -> X.cpp.dep.json
    obj_t = _mtime(obj)
    if obj_t is None:
        return True, "无 obj"
    dep_t = _mtime(dep)
    if dep_t is None:
        return True, "无 dep.json"
    try:
        with open(dep, "r", encoding="utf-8", errors="replace") as f:
            data = json.load(f).get("Data") or {}
    except (OSError, ValueError):
        return True, "dep.json 解析失败"

    deps = [data.get("Source", ""), data.get("PCH", "")]
    deps += data.get("Includes", []) or []
    for d in deps:
        if not d:
            continue
        t = _mtime(d)
        if t is not None and t > obj_t:
            return True, "依赖较新: " + os.path.basename(d)
    return False, "已是最新"


def compile_one(rsp_path):
    name = os.path.basename(rsp_path)
    t0 = time.time()
    try:
        p = subprocess.run([CL, "@" + rsp_path.replace("\\", "/")], cwd=ENGINE_SRC,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, errors="replace")
    except Exception as exc:  # noqa: BLE001
        return (name, -999, "LAUNCH EXCEPTION: %r" % (exc,), time.time() - t0)
    return (name, p.returncode, (p.stdout or "").strip(), time.time() - t0)


def do_compile(jobs, force):
    rsps = sorted(glob.glob(os.path.join(INTER, "*.obj.rsp")))
    todo = []
    skipped = 0
    for r in rsps:
        if force:
            todo.append(r)
            continue
        need, _why = outdated(r)
        if need:
            todo.append(r)
        else:
            skipped += 1
    log("[manual] 响应文件 %d 个：需编译 %d，已最新跳过 %d，并行度 %d"
        % (len(rsps), len(todo), skipped, jobs))
    if not todo:
        log("[manual] 编译阶段无需动作")
        return True, []

    ok = fail = 0
    failures = []
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        for i, (name, rc, out, dt) in enumerate(pool.map(compile_one, todo), 1):
            if rc == 0:
                ok += 1
                log("  [%2d/%d] OK   %-42s %5.1fs" % (i, len(todo), name, dt))
            else:
                fail += 1
                log("  [%2d/%d] FAIL %-42s rc=%s %5.1fs" % (i, len(todo), name, rc, dt))
                tail = "\n".join(out.splitlines()[-25:]) if out else "(无输出)"
                log("        ---- 输出 ----\n" + tail + "\n        ----------------")
                failures.append(name)
    log("[manual] 编译完成：成功 %d，失败 %d" % (ok, fail))
    return fail == 0, failures


def do_link(force):
    if not os.path.isfile(DLL_RSP):
        log("[manual] 找不到链接响应文件: %s" % DLL_RSP)
        return False

    with open(DLL_RSP, "r", encoding="utf-8", errors="replace") as f:
        body = f.read()
    for line in body.splitlines():
        if line.upper().startswith("/OUT:"):
            log("[manual] 链接输出目标: %s" % line)

    dll_t = _mtime(DLL_OUT)
    if dll_t is not None and not force:
        stale = []
        for obj in glob.glob(os.path.join(INTER, "*.obj")):
            t = _mtime(obj)
            if t is not None and t > dll_t:
                stale.append(os.path.basename(obj))
        if not stale:
            log("[manual] DLL 已是最新，跳过链接")
            return True
        log("[manual] 有 %d 个 obj 比 DLL 新，需要重新链接" % len(stale))

    log("[manual] 开始链接 ...")
    t0 = time.time()
    p = subprocess.run([LINK, "@" + DLL_RSP.replace("\\", "/")], cwd=ENGINE_SRC,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, errors="replace")
    out = (p.stdout or "").strip()
    log("[manual] link.exe rc=%s  耗时 %.1fs" % (p.returncode, time.time() - t0))
    if out:
        log(out)
    if p.returncode != 0:
        log("[manual] 链接失败。")
        return False
    log("[manual] 链接成功。")
    if os.path.isfile(DLL_OUT):
        log("[manual] DLL: %s  (%d bytes, %s)" % (
            DLL_OUT, os.path.getsize(DLL_OUT),
            time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(DLL_OUT)))))
    return True


def do_link_lib(force):
    """生成 import library（UBT 的 Link [x64] UnrealEditor-demo.lib 动作）。"""
    if not os.path.isfile(LIB_RSP):
        log("[manual] 跳过 import lib（无 %s）" % os.path.basename(LIB_RSP))
        return True
    lib_t = _mtime(LIB_OUT)
    dll_t = _mtime(DLL_OUT)
    if lib_t is not None and dll_t is not None and lib_t >= dll_t and not force:
        log("[manual] import lib 已是最新，跳过")
        return True
    log("[manual] 生成 import lib ...")
    p = subprocess.run([LINK, "/LIB", "@" + LIB_RSP.replace("\\", "/")], cwd=ENGINE_SRC,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, errors="replace")
    out = (p.stdout or "").strip()
    log("[manual] link.exe /LIB rc=%s" % p.returncode)
    if out:
        log(out)
    if p.returncode != 0:
        log("[manual] import lib 生成失败。")
        return False
    log("[manual] import lib 生成成功: %s" % LIB_OUT)
    return True


def do_write_metadata(force):
    """UBT 的 WriteMetadata 动作：它自己再起一个 dotnet 子进程，同样会被环境掐断，
    但由本脚本来执行这个子进程本身是正常的。"""
    if not os.path.isfile(TARGET_META):
        log("[manual] 跳过 target 元数据（无 TargetMetadata.dat）")
        return True
    tgt_t = _mtime(TARGET_FILE)
    meta_t = _mtime(TARGET_META) or 0.0
    dll_t = _mtime(DLL_OUT) or 0.0
    # UBT 只要目标产物比 .target 新就要重写元数据
    newest_in = max(meta_t, dll_t)
    if not force and tgt_t is not None and tgt_t >= newest_in:
        log("[manual] target 元数据已是最新，跳过")
        return True
    log("[manual] 生成 target 元数据 ...")
    p = subprocess.run([DOTNET, UBT_DLL, "-Mode=WriteMetadata",
                        "-Input=" + TARGET_META, "-Version=2"],
                       cwd=ENGINE_SRC, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, errors="replace")
    out = (p.stdout or "").strip()
    log("[manual] WriteMetadata rc=%s" % p.returncode)
    if out:
        log(out)
    if p.returncode != 0:
        log("[manual] target 元数据生成失败（不影响编辑器加载模块）")
        return False
    log("[manual] target 元数据生成成功: %s" % TARGET_FILE)
    return True


def main():
    global _logf
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--force", action="store_true", help="忽略增量判断，全量重编")
    ap.add_argument("--compile", action="store_true")
    ap.add_argument("--link", action="store_true")
    args = ap.parse_args()
    do_c = args.compile or not (args.compile or args.link)
    do_l = args.link or not (args.compile or args.link)

    _logf = open(LOG, "w", encoding="utf-8")
    log("========== ManualBuild 开始 %s ==========" % time.strftime("%Y-%m-%d %H:%M:%S"))
    good = True
    if do_c:
        good, _ = do_compile(args.jobs, args.force)
    if good and do_l:
        good = do_link(args.force)
    if good and do_l:
        good = do_link_lib(args.force)
    if good and do_l:
        do_write_metadata(args.force)   # 失败不算致命
    log("========== ManualBuild 结束，结果: %s ==========" % ("成功" if good else "失败"))
    _logf.close()
    sys.exit(0 if good else 1)


if __name__ == "__main__":
    main()
