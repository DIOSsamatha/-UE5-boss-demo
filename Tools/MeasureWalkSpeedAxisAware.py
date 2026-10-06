# -*- coding: utf-8 -*-
"""
MeasureWalkSpeedAxisAware.py
============================
第二版：按【主轴】测量原生地面速度（第一版有致命错误，见下）。

第一版为什么错
--------------
第一版固定采样 foot_l 的 **Y（前后）** 跨度。对 Jog_Fwd / Jog_Bwd 这是对的
（Y跨度 ≈ 112cm，脚步前后迈）。但对 **Strafe_Left/Right 完全错**：
横移的脚步是沿 **X（左右）** 迈的，Y 方向几乎不动 ——
实测 Strafe_Left 的 Y跨度只有 17.2cm，于是算出"原生速度 12cm/s"、
"推荐倍速 32 倍"这种荒谬结论。**用它去调参会把横移调成快进。**

第二版做法
----------
每条动画都**先找出它自己的主推进轴**（X 还是 Y）：
  1) 采样 foot_l / foot_r 的 X、Y、Z
  2) 比较 |X跨度| 与 |Y跨度|，大的那个 = 推进轴
  3) 只在【推进轴】上取支撑相后退速度
这样四条动画都用自己的真实轴，测出来的数才有意义。

另外顺带输出"抬腿高度"（Z跨度）：
  抬腿太低的动画（如 Strafe 只 11cm）看着像"贴地蹭"，观感上就是滑步，
  需要单独标出来。
"""
import unreal
import statistics

OUT = "K:/selfgame/demo/Tools/_walk_speed_axis.txt"
_lines = []
def print(*a, **k):        # noqa: A001
    _lines.append(" ".join(str(x) for x in a))
def flush():
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(_lines))

BASE = "/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations"
TARGETS = ["Jog_Fwd", "Jog_Bwd", "Strafe_Left", "Strafe_Right"]
N = 500

print("=" * 70)
print("四方向行走动画 · 原生地面速度（按主轴自适应测量）")
print("=" * 70)
print("")

speed = {}
for name in TARGETS:
    path = f"{BASE}/{name}.{name}"
    seq = unreal.load_object(None, path)
    if not seq:
        print("[MISS] %s\n" % name)
        continue

    L = float(seq.get_play_length())
    opt = unreal.AnimPoseEvaluationOptions()

    xs_l, ys_l, zs_l = [], [], []
    xs_r, ys_r, zs_r = [], [], []
    for i in range(N + 1):
        t = L * i / N
        pose = seq.get_anim_pose_at_time(t, opt)
        pl = unreal.AnimPoseExtensions.get_bone_pose(pose, "foot_l", unreal.AnimPoseSpaces.WORLD)
        pr = unreal.AnimPoseExtensions.get_bone_pose(pose, "foot_r", unreal.AnimPoseSpaces.WORLD)
        xs_l.append(float(pl.translation.x)); ys_l.append(float(pl.translation.y)); zs_l.append(float(pl.translation.z))
        xs_r.append(float(pr.translation.x)); ys_r.append(float(pr.translation.y)); zs_r.append(float(pr.translation.z))

    span_x = (max(xs_l) - min(xs_l)) + (max(xs_r) - min(xs_r))
    span_y = (max(ys_l) - min(ys_l)) + (max(ys_r) - min(ys_r))
    axis = "X" if span_x > span_y else "Y"
    prim_l = xs_l if axis == "X" else ys_l
    prim_r = xs_r if axis == "X" else ys_r
    zs = zs_l

    zmin, zmax = min(zs), max(zs)
    lift = zmax - zmin
    thr = zmin + lift * 0.30
    dt = L / N

    def retreat(prim):
        out = []
        for i in range(len(prim) - 1):
            if zs[i] <= thr and zs[i + 1] <= thr:
                d = (prim[i + 1] - prim[i]) / dt
                if d < 0:
                    out.append(-d)
        return out

    neg_l = retreat(prim_l)
    neg_r = retreat(prim_r)
    allneg = neg_l + neg_r
    if not allneg:
        print("【%s】主轴=%s 但支撑相无后退样本（不可用）\n" % (name, axis))
        continue

    med = statistics.median(allneg)
    speed[name] = med
    print("【%s】" % name)
    print("   时长 %.3fs   X跨度 %.1f   Y跨度 %.1f  → 主推进轴 = %s"
          % (L, span_x, span_y, axis))
    print("   抬腿高度(Z跨度) = %.1fcm %s" % (lift, "← 偏低，贴地蹭" if lift < 25 else ""))
    print("   支撑相后退速度 中位=%.1f  均值=%.1f  样本=%d"
          % (med, sum(allneg) / len(allneg), len(allneg)))
    print("   → 参考速度 = %.0f cm/s   （BOSS 满速 380 时倍速 = %.2f）"
          % (med, 380.0 / med if med > 0 else 0.0))
    print("")

print("=" * 70)
print("汇总 → 填进 BP_Boss 的 Boss|Animation")
print("=" * 70)
def g(k, d): return "%.0f" % speed.get(k, d)
print("BossJogReferenceSpeedFwd   = %s   <- Jog_Fwd" % g("Jog_Fwd", 417))
print("BossJogReferenceSpeedBwd   = %s   <- Jog_Bwd" % g("Jog_Bwd", 428))
print("BossJogReferenceSpeedLeft  = %s   <- Strafe_Left" % g("Strafe_Left", 330))
print("BossJogReferenceSpeedRight = %s   <- Strafe_Right" % g("Strafe_Right", 330))
flush()
