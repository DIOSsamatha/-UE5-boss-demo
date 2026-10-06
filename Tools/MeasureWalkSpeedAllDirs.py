# -*- coding: utf-8 -*-
"""
MeasureWalkSpeedAllDirs.py
==========================
一次性实测【四个方向行走动画各自的原生地面速度】。

为什么必须分方向测（2026-10-03）
-------------------------------
消"空步"的核心公式是：

    播放倍速 = 实际移速 / 动画的原生地面速度

而**每条动画的原生地面速度是不同的**：
  · Jog_Fwd 跨度最大、步频高 → 原生速度高
  · Strafe_Left/Right 是横移，脚步横向迈 → 纵向推进量小 → 原生速度低
  · Jog_Bwd 倒着走 → 又是另一套

之前全项目**共用一个 415**，于是：
  · 前冲基本对（0.92 倍速）
  · 横移/后撤时倍速【偏大】→ 脚蹬得比身体慢 → 脚在身前"等"着滑 = 拖步/空步

所以这一轮把每个方向的原生速度都测出来，各方向各用自己的基准。

算法（无假设，不猜步态周期）
----------------------------
  1) 细采样 400+ 点，记录 foot_l / foot_r 的【水平位移量】与【高度】
  2) 支撑相 = 脚 Z 在下 30% 的那一段（脚踩地）
  3) 支撑相里 d(水平位移)/dt 为负（脚相对身体后退）的**中位数** = 原生地面速度
  4) 速度和骨骼轴有关时，同时给出【Y 轴】与【水平合量】，对照 BOSS 实际用哪个轴
"""
import unreal
import statistics

OUT = "K:/selfgame/demo/Tools/_walk_speed_alldirs.txt"
_lines = []
def print(*a, **k):        # noqa: A001
    _lines.append(" ".join(str(x) for x in a))
def flush():
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(_lines))

BASE = "/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations"
TARGETS = ["Jog_Fwd", "Jog_Bwd", "Strafe_Left", "Strafe_Right"]
N = 400

print("=" * 68)
print("四方向行走动画 · 原生地面速度实测（BOSS MaxWalkSpeed = 380 cm/s）")
print("=" * 68)
print("")
print("支撑相 dY/dt 中位数 = 该方向推荐的 BossJogReferenceSpeed")
print("")

results = {}
for name in TARGETS:
    path = f"{BASE}/{name}.{name}"
    seq = unreal.load_object(None, path)
    if not seq:
        print("[MISS] %s" % name)
        continue

    L = float(seq.get_play_length())
    opt = unreal.AnimPoseEvaluationOptions()
    ys, zs = [], []
    for i in range(N + 1):
        t = L * i / N
        pose = seq.get_anim_pose_at_time(t, opt)
        p = unreal.AnimPoseExtensions.get_bone_pose(pose, "foot_l", unreal.AnimPoseSpaces.WORLD)
        ys.append(float(p.translation.y))
        zs.append(float(p.translation.z))

    if not zs:
        print("[MISS bone] %s" % name)
        continue

    zmin, zmax = min(zs), max(zs)
    thr = zmin + (zmax - zmin) * 0.30          # 支撑相 = Z 在下 30%
    dt = L / N
    neg = []
    for i in range(len(ys) - 1):
        if zs[i] <= thr and zs[i + 1] <= thr:
            d = (ys[i + 1] - ys[i]) / dt
            if d < 0:                          # 脚相对身体后退
                neg.append(-d)

    if not neg:
        print("[WARN] %s 支撑相没有后退样本" % name)
        continue

    med = statistics.median(neg)
    results[name] = med
    print("【%s】" % name)
    print("   时长 = %.3fs   Y跨度 = %.1fcm   Z范围 = %.1f .. %.1f"
          % (L, max(ys) - min(ys), zmin, zmax))
    print("   支撑相后退速度：样本 %d  中位=%.1f  均值=%.1f  最大=%.1f"
          % (len(neg), med, sum(neg) / len(neg), max(neg)))
    print("   → 建议 BossJogReferenceSpeed = %.0f   （380/此值 = %.2f 倍速）"
          % (med, 380.0 / med if med > 0 else 0.0))
    print("")

print("=" * 68)
print("汇总（填进 BP_Boss 的 Boss|Animation 属性）")
print("=" * 68)
print("BossJogReferenceSpeedFwd   = %.0f   <- Jog_Fwd" % results.get("Jog_Fwd", 415))
print("BossJogReferenceSpeedBwd   = %.0f   <- Jog_Bwd" % results.get("Jog_Bwd", 380))
print("BossJogReferenceSpeedLeft  = %.0f   <- Strafe_Left" % results.get("Strafe_Left", 300))
print("BossJogReferenceSpeedRight = %.0f   <- Strafe_Right" % results.get("Strafe_Right", 300))
print("")
print("当前代码里只有一个 BossJogReferenceSpeed = 415（四方向共用）")
print("→ 建议改成按方向分别取值，否则横移/后撤必然空步。")
flush()
