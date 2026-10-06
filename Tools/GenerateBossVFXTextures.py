# -*- coding: utf-8 -*-
"""
BOSS 特效贴图生成器（程序化，不依赖编辑器）

为什么不用 AI 出图：
  游戏特效贴图本质是「遮罩」，需要的是精确可控的数学形状 + 干净的 alpha 通道。
  AI 出图给不出干净 alpha、做不出严格对称、也不能参数化调宽度 —— 拿来当发光遮罩
  会出现边缘发脏、对不齐、换分辨率就糊的问题。
  所以这里用解析式（高斯 / 环形 / 星芒）直接算，512² 一张也就几十毫秒。

用法：
    <受管 python> Tools/GenerateBossVFXTextures.py
输出：
    Tools/VFXSource/*.png   （再由 Tools/SetupBossVFX.py 在编辑器里导入 + 建材质）
"""

import os
import sys

try:
    import numpy as np
    from PIL import Image
except ImportError:
    print("[X] 需要 numpy 与 pillow：")
    print('    pip install pillow numpy')
    sys.exit(1)

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "VFXSource")

SAVE8 = True  # 8 位足够：这些贴图最终都是当遮罩用，16 位只会让包变大


def _grid(size):
    """返回归一化坐标：x/y ∈ [-1, 1]，r = 到中心距离（1.0 = 贴图内切圆边界）"""
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    c = (size - 1) * 0.5
    x = (xx - c) / c
    y = (yy - c) / c
    r = np.sqrt(x * x + y * y)
    theta = np.arctan2(y, x)
    return x, y, r, theta


def save(name, value, rgb=None):
    """value = 亮度/alpha 遮罩（0~1）；rgb 为空时输出白色（由材质 Color 参数上色）"""
    value = np.clip(value, 0.0, 1.0)
    size = value.shape[0]

    if rgb is None:
        rgb = np.stack([value, value, value], axis=-1)

    rgba = np.zeros((size, size, 4), dtype=np.uint8)
    rgba[..., 0:3] = np.clip(rgb * 255.0, 0, 255).astype(np.uint8)
    rgba[..., 3] = (value * 255.0).astype(np.uint8)

    path = os.path.join(OUT_DIR, name + ".png")
    Image.fromarray(rgba, "RGBA").save(path, optimize=True)
    print("  [OK] %-22s %dx%d  %6.1f KB" % (name + ".png", size, size, os.path.getsize(path) / 1024.0))


# ------------------------------------------------------------------ 1. 弹体 / 拖尾的发光球
def make_glow(size=512):
    """
    双层高斯：外圈柔和光晕 + 中心一个过曝小核。
    拖尾的每一段残影都用它 —— 所以必须「中心亮、边缘平滑归零」，
    否则几十个球叠在一起会出现明显的硬边圆圈。
    """
    _, _, r, _ = _grid(size)

    halo = np.exp(-np.power(r / 0.45, 2.0))
    core = 0.60 * np.exp(-np.power(r / 0.13, 2.0))

    v = np.clip(halo + core, 0.0, 1.0)
    v = np.power(v, 0.85)          # 抬高暗部，尾巴末端才不会断续
    v = np.clip(v * 1.12, 0.0, 1.0)  # 中心小范围过曝
    return v


# ------------------------------------------------------------------ 2. 命中扩散环
def make_ring(size=512):
    """
    细环 + 内圈柔光 + 轻微辐条。
    辐条是为了让环在旋转时有「能量在流动」的感觉（纯圆环静止看很死板）。
    """
    _, _, r, theta = _grid(size)

    ring = np.exp(-np.power((r - 0.70) / 0.085, 2.0))
    inner = 0.22 * np.exp(-np.power((r - 0.52) / 0.30, 2.0))

    spokes = 0.88 + 0.12 * np.abs(np.cos(theta * 8.0))

    v = (ring * spokes) + inner
    v = np.power(np.clip(v, 0.0, 1.0), 0.9)
    v[r > 1.0] = 0.0               # 硬裁到内切圆，避免四个角出现方形残留
    return v


# ------------------------------------------------------------------ 3. 命中爆闪
def make_impact(size=512):
    """
    中心爆核 + 8 道细星芒。
    这道贴图是「命中」的即时反馈，只存在 0.1~0.2s，所以形状要够抢眼 ——
    纯圆形闪光在快节奏下几乎看不见，细长的星芒才能在余光里被捕捉到。

    ⚠ 星芒要「细」：|cos| 的指数给到 8，指数太小（3~4）会变成一片扇形光斑，
      八道芒糊在一起，看起来只是一坨发光的圆。
    """
    _, _, r, theta = _grid(size)

    core = np.exp(-np.power(r / 0.16, 2.0))
    rays = np.exp(-np.power(r / 0.65, 2.0)) * np.power(np.abs(np.cos(theta * 4.0)), 8.0)
    # 45° 方向补 4 道短芒：只有 8 道等长芒会显得很"人工"
    short_rays = 0.55 * np.exp(-np.power(r / 0.34, 2.0)) * np.power(np.abs(np.cos(theta * 4.0 + 0.7854)), 8.0)
    haze = 0.30 * np.exp(-np.power(r / 0.26, 2.0))

    v = np.clip(core + 1.05 * rays + short_rays + haze, 0.0, 1.0)
    v = np.power(v, 0.85)

    # 软圆盘衰减：让 alpha 在贴图边缘自然收到 0。
    # 直接 v[r>1]=0 会在对角线上留下可见的"截断直角"
    v *= np.clip(1.0 - np.power(r, 5.0), 0.0, 1.0)
    v[r > 1.0] = 0.0
    return v


# ------------------------------------------------------------------ 4. 地面预警圈
def make_telegraph(size=512):
    """
    外圈实线 + 内圈细线 + 24 段刻度 + 极淡填充。
    用来在地上画出「这招会打到这里」。刻度是给玩家的距离参照 ——
    只有一条圆环的话，玩家很难判断自己是不是刚好站在边缘上。
    """
    _, _, r, theta = _grid(size)

    outer = np.exp(-np.power((r - 0.92) / 0.035, 2.0))
    mid = 0.50 * np.exp(-np.power((r - 0.855) / 0.030, 2.0))
    tick = np.exp(-np.power((r - 0.775) / 0.070, 2.0)) * (np.cos(theta * 24.0) > 0.86).astype(np.float32)
    fill = 0.10 * np.exp(-np.power(r / 0.85, 4.0))

    v = np.clip(outer + mid + 0.80 * tick + fill, 0.0, 1.0)
    v[r > 1.0] = 0.0
    return v


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    print("[VFX] 生成特效贴图 -> %s" % OUT_DIR)

    save("T_VFX_Glow", make_glow())
    save("T_VFX_Ring", make_ring())
    save("T_VFX_Impact", make_impact())
    save("T_VFX_Telegraph", make_telegraph())

    print("")
    print("[OK] 完成。接着在编辑器里跑 Tools/SetupBossVFX.py 导入并建材质。")
    print("     （C++ 按 /Game/Boss/VFX/ 的约定路径加载；不跑也能打，只是弹幕没有弹道）")


if __name__ == "__main__":
    main()
