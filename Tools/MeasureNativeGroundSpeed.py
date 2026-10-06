# -*- coding: utf-8 -*-
# 精确实测原地(in-place)行走动画的【原生地面速度】= 脚在【支撑相】相对身体向后蹬的速度。
#
# 为什么必须重测：空步(feet sliding)的根因就是"播放倍速 = 实际移速 / 原生速度"里的
# 【原生速度】不对。之前用"脚骨位移跨度/时长"算成 370，但如果那个值偏大，
# 倍速就会偏小 → 脚向后蹬的量跟不上身体前进量 → 脚在地上往前滑 = 空步。
#
# 无假设的算法（不猜步态周期）：
#   1) 细采样 foot_l 的 Y(前后) 与 Z(高度)
#   2) 支撑相 = 脚 Z 接近最低（踩地）的那一段
#   3) 支撑相里 dY/dt 取负（脚相对身体后退）的【中位数】= 原生地面速度
import unreal, statistics

OUT = "K:/selfgame/demo/Tools/_native_speed.txt"
_lines = []
def print(*a, **k):        # noqa: A001  屏蔽掉，重定向到文件
    _lines.append(" ".join(str(x) for x in a))
def flush():
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(_lines))

SEQ = "/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations/Jog_Fwd.Jog_Fwd"
seq = unreal.load_object(None, SEQ)
if not seq:
    print("!! 序列没找到", SEQ)
    flush()
    raise SystemExit(1)

L = float(seq.get_play_length())
N = 400
opt = unreal.AnimPoseEvaluationOptions()

ys, zs = [], []
for i in range(N + 1):
    t = L * i / N
    pose = seq.get_anim_pose_at_time(t, opt)
    p = unreal.AnimPoseExtensions.get_bone_pose(pose, "foot_l", unreal.AnimPoseSpaces.WORLD)
    loc = p.translation
    ys.append(float(loc.y))
    zs.append(float(loc.z))

print("序列时长 = %.3fs  采样 %d 点" % (L, N + 1))
print("Y(前后) 范围 = %.1f .. %.1f  (跨度 %.1fcm)" % (min(ys), max(ys), max(ys) - min(ys)))
print("Z(高度) 范围 = %.1f .. %.1f" % (min(zs), max(zs)))

dt = L / N
# 支撑相：Z 在下 30% 区间（脚踩地）
zlo = min(zs) + 0.30 * (max(zs) - min(zs))

# 中心差分求 dY/dt
vy = []
for i in range(1, len(ys) - 1):
    vy.append((ys[i + 1] - ys[i - 1]) / (2.0 * dt))

# 支撑相里"脚相对身体后退"(vy<0，即向 -Y) 的速度
back = []
for i in range(1, len(zs) - 1):
    if zs[i] <= zlo and vy[i - 1] < 0:
        back.append(-vy[i - 1])

if not back:
    print("!! 没取到支撑相样本")
    flush()
else:
    G = statistics.median(back)
    print("")
    print("支撑相样本数 = %d" % len(back))
    print("支撑相后退速度: 中位=%.1f  均值=%.1f  min=%.1f  max=%.1f cm/s"
          % (G, statistics.mean(back), min(back), max(back)))
    print("")
    print(">>> 原生地面速度 G = %.1f cm/s" % G)
    print(">>> 满速 380 时应设的播放倍速 = %.2f" % (380.0 / G if G > 0 else 0))
    print(">>> 建议 BossJogReferenceSpeed = %.0f" % G)
flush()
