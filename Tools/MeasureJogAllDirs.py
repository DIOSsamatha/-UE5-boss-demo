# -*- coding: utf-8 -*-
# 量 Jog 四向（Fwd/Bwd/Left/Right）的"原生地面速度"：脚骨在支撑相向后/侧向的速度峰值。
# 用于判断方向化 Jog 各自的脚速是否一致——不一致就会在绕圈/后撤时滑步。
import unreal, traceback

OUT = "K:/selfgame/demo/Tools/_measure_alldirs.txt"
buf = []
def log(s):
    buf.append(str(s)); print(s, flush=True)
def flush():
    try:
        with open(OUT, "w", encoding="utf-8") as f:
            f.write("\n".join(buf) + "\n")
    except Exception as e:
        print("write err:", e, flush=True)

try:
    opts = unreal.AnimPoseEvaluationOptions()
    def measure(path):
        seq = unreal.load_asset(path)
        if not seq:
            return None, "load fail"
        dur = float(seq.get_play_length())
        N = 120
        dt = dur / float(N)
        xs=[]; ys=[]; zs=[]
        for i in range(N):
            t = (i / float(N)) * dur
            pose = seq.get_anim_pose_at_time(t, opts)
            tr = unreal.AnimPoseExtensions.get_bone_pose(pose, "foot_l", unreal.AnimPoseSpaces.WORLD)
            try: loc = tr.get_location()
            except Exception: loc = tr.translation
            xs.append(float(loc.x)); ys.append(float(loc.y)); zs.append(float(loc.z))
        # 逐采样中心差分水平速度
        vx=[]; vy=[]
        for i in range(1, len(xs)-1):
            vx.append((xs[i+1]-xs[i-1])/(2.0*dt))
            vy.append((ys[i+1]-ys[i-1])/(2.0*dt))
        # 水平速度幅值峰值（≈ 原生地面速度）
        speeds = [ (vx[i]**2 + vy[i]**2)**0.5 for i in range(len(vx)) ]
        peak = max(speeds) if speeds else 0.0
        # 落在下 30% 高度（踩地）时的水平速度均值
        zmin,zmax = min(zs), max(zs)
        zlo = zmin + 0.30*(zmax-zmin)
        stance = [speeds[i] for i in range(len(speeds)) if zs[i+1] <= zlo]
        import statistics
        stance_mean = statistics.mean(stance) if stance else 0.0
        # 前进轴：看 X 还是 Y 跨度大
        ax = "X" if (max(xs)-min(xs)) >= (max(ys)-min(ys)) else "Y"
        return (peak, stance_mean, ax, max(xs)-min(xs), max(ys)-min(ys)), None

    for name in ("Fwd","Bwd","Left","Right"):
        p = "/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations/Jog_%s.Jog_%s" % (name, name)
        res, err = measure(p)
        if err:
            log("Jog_%s: %s" % (name, err))
        else:
            peak, sm, ax, xr, yr = res
            log("Jog_%-4s peakHorizSpeed=%.1f  stanceMean=%.1f  domAxis=%s  Xrange=%.1f Yrange=%.1f" % (
                name, peak, sm, ax, xr, yr))
    log("=== done ===")
except Exception:
    buf.append(traceback.format_exc()); print(traceback.format_exc(), flush=True)
finally:
    flush()
