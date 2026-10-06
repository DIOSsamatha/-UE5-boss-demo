# -*- coding: utf-8 -*-
"""
从参考 Demo 视频抽帧，供"反馈链 / UI 布局"视觉对标使用。

用法:
    python Tools/ExtractReferenceFrames.py                 # 默认抽 16 帧
    python Tools/ExtractReferenceFrames.py 24              # 抽 24 帧
    python Tools/ExtractReferenceFrames.py 12 60 180       # 只抽 60s~180s 区间的 12 帧

输出: Docs/ReferenceFrames/ref_000.jpg ...
本项目不做代码构建，仅产出图片，可安全反复运行。
"""
import os
import re
import subprocess
import sys

import imageio_ffmpeg

FFMPEG = imageio_ffmpeg.get_ffmpeg_exe()

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_DIR = os.path.join(ROOT, "Docs", "ReferenceFrames")

CANDIDATES = [
    "【26届战斗策划求职】UE5高频交互Boss战Demo（已入职）.flv",
    "【26届战斗策划求职】UE5高频交互Boss战Demo（已入职）.mp4",
]


def find_video():
    for name in CANDIDATES:
        p = os.path.join(ROOT, name)
        if os.path.exists(p):
            return p
    # 兜底：根目录里任何常见视频后缀
    for f in os.listdir(ROOT):
        if f.lower().endswith((".flv", ".mp4", ".mkv", ".mov", ".webm")):
            return os.path.join(ROOT, f)
    return None


def probe_duration(path):
    """从 ffmpeg -i 的 stderr 里解析时长（秒）。失败返回 None。"""
    res = subprocess.run([FFMPEG, "-i", path], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, encoding="utf-8", errors="ignore")
    m = re.search(r"Duration:\s*(\d+):(\d+):(\d+\.?\d*)", res.stderr)
    if not m:
        return None
    h, mi, s = int(m.group(1)), int(m.group(2)), float(m.group(3))
    return h * 3600 + mi * 60 + s


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 16
    start = float(sys.argv[2]) if len(sys.argv) > 2 else None
    end = float(sys.argv[3]) if len(sys.argv) > 3 else None

    video = find_video()
    if not video:
        print("[Extract] 未找到参考视频（放到工程根目录）")
        return 1

    duration = probe_duration(video)
    print("[Extract] 视频:", os.path.basename(video))
    print("[Extract] 时长:", duration)

    if duration is None:
        duration = 300.0

    t0 = 0.0 if start is None else max(0.0, start)
    t1 = duration if end is None else min(duration, end)
    # 首尾各留 3% 余量，避开片头黑帧与片尾字幕
    span = (t1 - t0) * 0.94
    t0 += (t1 - t0) * 0.03

    os.makedirs(OUT_DIR, exist_ok=True)
    saved = []
    for i in range(count):
        t = t0 + span * (i / max(1, count - 1))
        out = os.path.join(OUT_DIR, "ref_%03d.jpg" % i)
        cmd = [FFMPEG, "-y", "-ss", "%.2f" % t, "-i", video,
               "-frames:v", "1", "-vf", "scale=960:-1", "-q:v", "4", out]
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if os.path.exists(out):
            saved.append(out)
            print("[Extract] %03d  t=%.1fs -> %s" % (i, t, out))

    print("[Extract] 完成，共 %d 帧 -> %s" % (len(saved), OUT_DIR))
    return 0


if __name__ == "__main__":
    sys.exit(main())
