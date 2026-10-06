# -*- coding: utf-8 -*-
# 探 BP_Boss 的 SkeletalMesh 组件相对旋转，判断 BOSS 网格是否带 90° Yaw 偏移。
# 这决定 Jog 动画的"前向(Y)"是否对齐 BOSS 角色的"前向(X)"。
import unreal, traceback

OUT = "K:/selfgame/demo/Tools/_probe_mesh.txt"
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
    # 候选 BP_Boss 路径
    candidates = [
        "/Game/FightBoss/Blueprints/BP_Boss.BP_Boss",
        "/Game/Boss/BP_Boss.BP_Boss",
        "/Game/Blueprints/BP_Boss.BP_Boss",
    ]
    bp = None
    for c in candidates:
        a = unreal.load_asset(c)
        if a:
            bp = a; log("loaded BP: %s" % c); break
    if not bp:
        log("BP_Boss not found in candidates")

    if bp:
        # 通过 SCS 拿 mesh 组件
        scs = bp.get_editor_property("simple_construction_script") if hasattr(bp, "get_editor_property") else None
        log("scs=%s" % scs)
        if scs:
            nodes = scs.get_all_nodes()
            for n in nodes:
                try:
                    comp = n.get_actual_component_template() if hasattr(n, "get_actual_component_template") else n.component_template
                    cn = comp.get_class().get_name() if comp else "?"
                    log("node comp class=%s" % cn)
                    if "SkeletalMesh" in cn:
                        rot = comp.get_editor_property("relative_rotation") if hasattr(comp, "get_editor_property") else None
                        loc = comp.get_editor_property("relative_location") if hasattr(comp, "get_editor_property") else None
                        log("  relative_rotation=%s" % rot)
                        log("  relative_location=%s" % loc)
                        sm = comp.get_editor_property("skeletal_mesh") if hasattr(comp, "get_editor_property") else None
                        log("  skeletal_mesh=%s" % (sm.get_path_name() if sm else None))
                except Exception as e:
                    log("  node err: %r" % e)
    log("=== done ===")
except Exception:
    import traceback as tb
    buf.append(tb.format_exc()); print(tb.format_exc(), flush=True)
finally:
    flush()
