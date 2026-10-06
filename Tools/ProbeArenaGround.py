# -*- coding: utf-8 -*-
"""
探针：FightArena 里"角色踩不到地"到底是踩在什么上面。

要回答的问题（都是实测数值，不靠猜）：
  1. 竞技场网格的【简单碰撞】到底是什么：0 个元素？几个盒子？一个大凸包？
  2. 在选定 X/Y 正下方打【复合碰撞】垂直射线（bTraceComplex=True，永远打三角面）
     → "视觉大理石的真实高度"。
     同一位置再打【简单碰撞】射线 → "角色实际会站在的高度"。
     两条数值一减，就是"悬空高度"。
  3. 改完碰撞后再跑一遍，简单射线应该也能打到大理石（= 改造生效的直接证据）。

⚠ UE5.7 的 Python 里静态方法库的模块名不固定（`unreal.KismetSystemLibrary` 在本版本
  直接 AttributeError），所以这里【先扫描 dir(unreal) 找哪个类有线射线方法】，再调用。

跑法（不用开编辑器，第二个进程只读）：
  "K:/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "K:/selfgame/demo/demo.uproject" \
      -run=pythonscript -script="K:/selfgame/demo/Tools/ProbeArenaGround.py" \
      -unattended -NoLiveCoding -NoSound -NullRHI
输出：K:/selfgame/demo/Saved/probe_arena_ground.txt
"""
import unreal

LEVEL = "/Game/Fab/FightArena"
ARENA_LABEL = "Arena_StaticMesh"
OUT = r"K:/selfgame/demo/Saved/probe_arena_ground.txt"

# 采样点：玩家出生点、BOSS 出生点、场地中心、四角
SAMPLES = [(0, 0), (-8, 3849), (8, -4706), (0, 2500), (0, -2500),
           (0, 6000), (0, -6000), (-2500, 0), (2500, 0)]

_lines = []


def emit(s):
    _lines.append(str(s))
    unreal.log(s)


def save():
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(_lines))
    unreal.log("[probe] 报告写入 %s" % OUT)


def step(label, fn):
    try:
        return fn()
    except Exception as exc:
        emit("[probe] 步骤失败「%s」：%s: %s" % (label, type(exc).__name__, exc))
        return None


def getp(obj, *names):
    for n in names:
        try:
            return obj.get_editor_property(n)
        except Exception:
            continue
    return None


# ----------------------------------------------------------------------------
# 找到真正带垂直射线的库
# ----------------------------------------------------------------------------
def find_trace_libs():
    found = []
    for name in dir(unreal):
        try:
            obj = getattr(unreal, name)
        except Exception:
            continue
        for meth in ("line_trace", "line_trace_single"):
            if hasattr(obj, meth):
                found.append((name, meth))
                break
    return found


_TRACE_PICK = None


def trace_once(world, x, y, z0, z1, complex_):
    global _TRACE_PICK
    start = unreal.Vector(float(x), float(y), float(z0))
    end = unreal.Vector(float(x), float(y), float(z1))
    libs = find_trace_libs()
    if not libs:
        return None, "找不到任何带 line_trace 的 unreal 子模块"

    # 首选上一次成功的写法
    order = sorted(libs, key=lambda t: 0 if t == _TRACE_PICK else 1)
    errs = []
    for (mod, meth) in order:
        fn = getattr(getattr(unreal, mod), meth)
        forms = [
            ("%s.%s(channel,complex,[])" % (mod, meth),
             lambda f=fn: f(world, start, end, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
                            complex_, [], unreal.DrawDebugTrace.NONE, True)),
            ("%s.%s(channel,complex,[]) 无 ignore" % (mod, meth),
             lambda f=fn: f(world, start, end, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
                            complex_, [], unreal.DrawDebugTrace.NONE)),
            ("%s.%s(channel,complex)" % (mod, meth),
             lambda f=fn: f(world, start, end, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1,
                            complex_)),
        ]
        for label, call in forms:
            try:
                r = call()
                _TRACE_PICK = (mod, meth)
                return label, r
            except Exception as exc:
                errs.append("%s -> %s: %s" % (label, type(exc).__name__, exc))
    return None, " | ".join(errs[-3:])


def unpack_trace(r):
    if r is None:
        return None, None
    if isinstance(r, (tuple, list)):
        if len(r) == 0:
            return None, None
        if len(r) == 1:
            return bool(r[0]), None
        return bool(r[0]), r[1]
    return bool(r), None


def hit_info(hit):
    if hit is None:
        return "无命中结构"
    loc = getp(hit, "location", "Location")
    nrm = getp(hit, "normal", "impact_normal", "Normal")
    comp = getp(hit, "component", "Component")
    label = "?"
    try:
        owner = comp.get_owner()
        label = owner.get_actor_label() if owner else "?"
    except Exception:
        pass
    z = loc.z if loc is not None else float("nan")
    nz = nrm.z if nrm is not None else float("nan")
    return "Z=%.1f 法线Z=%.3f 命中=%s" % (z, nz, label)


def main():
    emit("[probe] ===== 竞技场地面体检 %s =====" % LEVEL)

    libs = find_trace_libs()
    emit("[probe] 带射线的 unreal 子模块：%s" % (libs if libs else "无"))

    step("加载关卡", lambda: unreal.EditorLevelLibrary.load_level(LEVEL))
    world = step("取世界", lambda: unreal.EditorLevelLibrary.get_editor_world())
    if world is None:
        emit("[probe] ✗ 拿不到 editor world，退出")
        save()
        return

    actors = step("列 Actor", lambda: unreal.EditorLevelLibrary.get_all_level_actors()) or []
    emit("[probe] 关卡 Actor 共 %d 个" % len(actors))

    arena = None
    for a in actors:
        try:
            loc = a.get_actor_location()
            sc = a.get_actor_scale3d()
            emit("[probe]   %-28s label=%-22s loc=(%.0f,%.0f,%.0f) scale=(%.3f,%.3f,%.3f)" % (
                a.get_class().get_name(), a.get_actor_label(),
                loc.x, loc.y, loc.z, sc.x, sc.y, sc.z))
        except Exception:
            pass
        try:
            if a.get_actor_label() == ARENA_LABEL:
                arena = a
        except Exception:
            pass

    # ---------------- 简单碰撞几何 ----------------
    if arena is not None:
        smc = step("取 StaticMeshComponent", lambda: arena.get_editor_property("static_mesh_component"))
        mesh = None
        if smc is not None:
            mesh = step("取 StaticMesh", lambda: smc.get_editor_property("static_mesh"))
            for p in ("collision_enabled", "collision_profile_name", "use_complex_as_simple_collision"):
                emit("[probe] 组件.%s = %s" % (p, getp(smc, p)))
        if mesh is None:
            mesh = step("直接读 static_mesh", lambda: arena.get_editor_property("static_mesh"))

        if mesh is not None:
            for lod in (0, 1):
                emit("[probe] LOD%d 三角数 = %s" % (
                    lod, step("tri%d" % lod, lambda l=lod: mesh.get_num_triangles(l))))
            bs = getp(mesh, "body_setup", "BodySetup")
            if bs is None:
                emit("[probe] ⚠ 拿不到 body_setup")
            else:
                emit("[probe] collision_trace_flag = %s" % getp(
                    bs, "collision_trace_flag", "CollisionTraceFlag"))
                agg = getp(bs, "agg_geom", "AggGeom")
                if agg is None:
                    emit("[probe] ⚠ 拿不到 agg_geom")
                else:
                    for key in ("box_elems", "sphere_elems", "sphyl_elems", "convex_elems"):
                        arr = getp(agg, key)
                        if arr is None:
                            emit("[probe] agg.%s = 不可读" % key)
                            continue
                        emit("[probe] agg.%s = %d 个" % (key, len(arr)))
                        for i, el in enumerate(list(arr)[:8]):
                            # FKBoxElem / FKConvexElem 的尺寸字段名不统一，逐个试
                            box = getp(el, "elem_box", "ElemBox")
                            if box is not None:
                                try:
                                    emit("[probe]      [%d] elem_box min=(%.0f,%.0f,%.0f) max=(%.0f,%.0f,%.0f)" % (
                                        i, box.min.x, box.min.y, box.min.z,
                                        box.max.x, box.max.y, box.max.z))
                                except Exception:
                                    emit("[probe]      [%d] elem_box=%s" % (i, box))
                            vd = getp(el, "vertex_data")
                            if vd is not None:
                                try:
                                    zs = [v.z for v in vd]
                                    xs = [v.x for v in vd]
                                    ys = [v.y for v in vd]
                                    emit("[probe]      [%d] 顶点数=%d  X[%.0f,%.0f] Y[%.0f,%.0f] Z[%.0f,%.0f]" % (
                                        i, len(vd), min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))
                                except Exception as exc:
                                    emit("[probe]      [%d] 顶点读取失败 %s" % (i, exc))

    # ---------------- 垂直射线：视觉地板 vs 简单碰撞地板 ----------------
    emit("[probe] ---- 垂直射线（起点 Z=+6000，终点 Z=-800）----")
    form_used = None
    for (x, y) in SAMPLES:
        for complex_ in (True, False):
            label, r = trace_once(world, x, y, 6000.0, -800.0, complex_)
            if label is None:
                emit("[probe] (%6d,%6d) 复合=%-5s ✗ %s" % (x, y, complex_, r))
                continue
            form_used = label
            ok, hit = unpack_trace(r)
            emit("[probe] (%6d,%6d) 复合=%-5s %s  %s" % (
                x, y, complex_, "有地板" if ok else "无地板", hit_info(hit)))
    emit("[probe] 可用射线写法 = %s" % form_used)

    save()


main()
