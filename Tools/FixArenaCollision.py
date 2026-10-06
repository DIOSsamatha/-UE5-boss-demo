# -*- coding: utf-8 -*-
"""
把 Fab 竞技场网格的碰撞从【一个包住整场的大凸包】改成【复合碰撞当简单碰撞用】。

为什么：
    `Tools/ProbeArenaGround.py` 实测到 —— 这份竞技场网格的简单碰撞只有
        agg.box_elems=0 / sphere=0 / sphyl=0 / convex_elems=**1**
    也就是【一个凸包罩住整个 57m × 144m 的竞技场】。
    凸包会把所有凹进去的地方"填平"（走道、台阶、柱廊之间全被连成一个斜面），
    角色胶囊被它托住时，脚就停在【比大理石地面高好几米的虚拟斜面】上 —— 这就是"悬空"。

    而凸包本身是美术随手导出的粗糙近似，不可能靠位移/贴地代码修好：
    只要凸包还在，角色每帧都会被它重新托回那个高度。

修法：
    把 mesh 的 BodySetup.CollisionTraceFlag 设成 CTF_UseComplexAsSimple
    （编辑器里对应 Static Mesh 资产 → Collision → Collision Complexity →
      "Use Complex Collision As Simple"）：
    简单查询（胶囊体扫掠 / 射线）改走【三角面】，角色就踏在真正的大理石上，
    表面细节（台阶、平台、栏杆）也都是真实几何。

跑法（不用开编辑器）：
    "K:/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "K:/selfgame/demo/demo.uproject" \
        -run=pythonscript -script="K:/selfgame/demo/Tools/FixArenaCollision.py" \
        -unattended -NoLiveCoding -NoSound -NullRHI
"""
import unreal

ARENA_MESH = ("/Game/Fab/Boss_Fight_Arena_PVP_Arena/boss_fight_arenapvp_arena/"
              "StaticMeshes/boss_fight_arenapvp_arena")
OUT = r"K:/selfgame/demo/Saved/fix_arena_collision.txt"

_lines = []


def emit(s):
    _lines.append(str(s))
    unreal.log(s)


def save():
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(_lines))
    unreal.log("[fix] 报告写入 %s" % OUT)


def step(label, fn):
    try:
        return fn()
    except Exception as exc:
        emit("[fix] 步骤失败「%s」：%s: %s" % (label, type(exc).__name__, exc))
        return None


def getp(obj, *names):
    for n in names:
        try:
            return obj.get_editor_property(n)
        except Exception:
            continue
    return None


def report(mesh, tag):
    bs = getp(mesh, "body_setup", "BodySetup")
    if bs is None:
        emit("[fix] %s ⚠ 拿不到 body_setup" % tag)
        return
    flag = getp(bs, "collision_trace_flag", "CollisionTraceFlag")
    emit("[fix] %s collision_trace_flag = %s" % (tag, flag))
    agg = getp(bs, "agg_geom", "AggGeom")
    if agg is not None:
        parts = []
        for key in ("box_elems", "sphere_elems", "sphyl_elems", "convex_elems"):
            arr = getp(agg, key)
            parts.append("%s=%s" % (key, len(arr) if arr is not None else "?"))
        emit("[fix] %s 简单碰撞元素：%s" % (tag, ", ".join(parts)))


def main():
    emit("[fix] ===== 竞技场碰撞改造 %s =====" % ARENA_MESH)

    mesh = step("加载竞技场网格", lambda: unreal.load_asset(ARENA_MESH))
    if mesh is None:
        emit("[fix] ✗ 加载失败")
        save()
        return

    for lod in (0, 1, 2):
        n = step("三角数 LOD%d" % lod, lambda l=lod: mesh.get_num_triangles(l))
        if n:
            emit("[fix] LOD%d 三角数 = %s" % (lod, n))

    report(mesh, "改前")

    bs = getp(mesh, "body_setup", "BodySetup")
    if bs is None:
        emit("[fix] ✗ 拿不到 body_setup，无法改")
        save()
        return

    target = step("读枚举 CTF_USE_COMPLEX_AS_SIMPLE",
                  lambda: unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
    if target is None:
        emit("[fix] ✗ 拿不到枚举值")
        save()
        return
    emit("[fix] 目标 flag = %s" % target)

    ok = step("写 collision_trace_flag",
              lambda: bs.set_editor_property("collision_trace_flag", target))
    if ok is False:
        step("写 CollisionTraceFlag",
             lambda: bs.set_editor_property("CollisionTraceFlag", target))
    emit("[fix] 写入完成")

    report(unreal.load_asset(ARENA_MESH), "改后（重新取资产）")

    # 把资产标记脏 + 存盘（不存盘的话改动只活在内存里）
    step("标记脏", lambda: mesh.modify())
    saved = step("保存资产", lambda: unreal.EditorAssetLibrary.save_loaded_asset(mesh))
    emit("[fix] save_loaded_asset = %s" % saved)
    saved2 = step("再存一次(按路径)", lambda: unreal.EditorAssetLibrary.save_asset(ARENA_MESH, False))
    emit("[fix] save_asset = %s" % saved2)

    emit("[fix] ===== 完成；必须【重启编辑器】或用 Live Coding 编译后再测 =====")
    save()


main()
