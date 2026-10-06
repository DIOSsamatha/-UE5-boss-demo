# -*- coding: utf-8 -*-
# 测 Jog_Fwd 脚骨在一个循环里的水平位移跨度 = 步幅，
# 原生地面速度 G = 2 * 步幅 / 循环时长。
# 全脚本包在 try/finally 里，任何异常都写进 Tools/_measure_foot.txt，避免静默失败。
import unreal, traceback

OUT = "K:/selfgame/demo/Tools/_measure_foot.txt"
buf = []
def log(s):
    buf.append(str(s))
    print(s, flush=True)

def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as f:
            f.write("\n".join(buf))
            f.write("\n")
    except Exception as e:
        print("write err:", e, flush=True)

try:
    seq_path = "/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations/Jog_Fwd.Jog_Fwd"
    seq = unreal.load_asset(seq_path)
    if not seq:
        log("FAIL load " + seq_path)
        raise SystemExit(1)

    try:
        dur = float(seq.get_play_length())
    except Exception as e:
        log("dur err %s" % e)
        dur = 2.0
    log("loaded dur=%.4f" % dur)

    opts = unreal.AnimPoseEvaluationOptions()
    log("opts ok, type=%s" % type(opts))

    # 取一帧 pose
    pose0 = None
    try:
        pose0 = seq.get_anim_pose_at_time(0.0, opts)
        log("get_anim_pose_at_time(return) ok type=%s" % type(pose0))
    except Exception as e:
        log("get_anim_pose_at_time(return) err: %s" % e)
        try:
            pose0 = unreal.AnimPose()
            seq.get_anim_pose_at_time(0.0, opts, pose0)
            log("get_anim_pose_at_time(out) ok")
        except Exception as e2:
            log("get_anim_pose_at_time(out) err: %s" % e2)
            raise

    # 枚举骨名
    bones = []
    try:
        bones = unreal.AnimPoseExtensions.get_bone_names(pose0)
        log("get_bone_names(return) ok n=%d" % len(bones))
    except Exception as e:
        log("get_bone_names(return) err: %s" % e)
        try:
            tmp = []
            unreal.AnimPoseExtensions.get_bone_names(pose0, tmp)
            bones = tmp
            log("get_bone_names(out) ok n=%d" % len(tmp))
        except Exception as e2:
            log("get_bone_names(out) err: %s" % e2)

    foot_like = [str(b) for b in bones if "foot" in str(b).lower() or "ball" in str(b).lower() or "toe" in str(b).lower()]
    log("bone count=%d foot-like=%s" % (len(bones), foot_like))

    bone = None
    for key in ("foot_l", "ball_l", "toe_l"):
        for b in foot_like:
            if b.lower() == key:
                bone = b
                break
        if bone:
            break
    if bone is None:
        for b in foot_like:
            if "l" in b.lower():
                bone = b
                break
    if bone is None and foot_like:
        bone = foot_like[0]
    if bone is None:
        bone = "root"
    log("using bone: %s" % bone)

    N = 120
    xs = []; ys = []; zs = []
    for i in range(N):
        t = (i / float(N)) * dur
        pose = seq.get_anim_pose_at_time(t, opts)
        tr = unreal.AnimPoseExtensions.get_bone_pose(pose, bone, unreal.AnimPoseSpaces.WORLD)
        try:
            loc = tr.get_location()
        except Exception:
            loc = tr.translation
        xs.append(float(loc.x)); ys.append(float(loc.y)); zs.append(float(loc.z))

    log("samples=%d" % len(xs))
    if len(xs) < 4:
        log("too few samples")
        raise SystemExit(1)

    step_x = max(xs) - min(xs)
    step_y = max(ys) - min(ys)
    log("x range=%.2f  y range=%.2f" % (step_x, step_y))
    log("z range=%.2f  z min=%.2f z max=%.2f" % (max(zs)-min(zs), min(zs), max(zs)))

    dt = dur / float(N)
    # 前向轴 = Y。计算每采样点 Y 方向中心差分速度。
    vys = []
    for i in range(1, len(ys) - 1):
        vys.append((ys[i + 1] - ys[i - 1]) / (2.0 * dt))

    # 探测真实步态周期：脚最靠前(Y 局部最大) 的间隔
    peaks = []
    for i in range(2, len(ys) - 2):
        if ys[i] > ys[i - 1] and ys[i] >= ys[i + 1] and ys[i] > ys[i - 2] and ys[i] >= ys[i + 2]:
            peaks.append((i, ys[i]))
    if len(peaks) >= 2:
        periods = [(peaks[k + 1][0] - peaks[k][0]) * dt for k in range(len(peaks) - 1)]
        gait = sum(periods) / len(periods)
    else:
        gait = dur
    log("foot-strike peaks=%d  gait period=%.3fs (asset len=%.2f)" % (len(peaks), gait, dur))

    # 支撑相判定：脚最低（z 在下 15% 区间，真正踩地）且正在向后(vy<0)
    zmin, zmax = min(zs), max(zs)
    zlo = zmin + 0.15 * (zmax - zmin)
    stance_v = []
    for i in range(1, len(ys) - 1):
        if zs[i] <= zlo and vys[i - 1] < 0:
            stance_v.append(-vys[i - 1])
    import statistics
    S = statistics.mean(stance_v) if stance_v else 0.0
    log("DEEP-stance backward-speed mean=%.2f n=%d min=%.2f max=%.2f" % (
        S, len(stance_v), min(stance_v) if stance_v else 0, max(stance_v) if stance_v else 0))

    # 交叉：S 也 = 单步幅 / 支撑相时长。这里用 deep-stance 占比估支撑相时长。
    deep_frac = len(stance_v) / float(len(ys) - 2) if (len(ys) - 2) > 0 else 0.0
    T_stance = deep_frac * gait
    step_len = (max(ys) - min(ys))  # 一个步态里脚的前后跨度≈步幅
    G2 = step_len / T_stance if T_stance > 0 else 0.0
    log("deep-stance frac=%.3f  T_stance=%.3fs  step_len=%.2f  -> G2=%.2f" % (deep_frac, T_stance, step_len, G2))

    log(">>> SUGGESTED BossJogReferenceSpeed S=%.2f  rate_at_380=%.3f  rate_at_250=%.3f" % (
        S, 380.0 / S if S > 0 else 0, 250.0 / S if S > 0 else 0))

except SystemExit:
    pass
except Exception:
    log("UNCAUGHT EXCEPTION:")
    buf.append(traceback.format_exc())
    print(traceback.format_exc(), flush=True)
finally:
    flush()
