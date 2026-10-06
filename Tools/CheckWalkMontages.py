# -*- coding: utf-8 -*-
# 校验行走蒙太奇是否真的生成成功、且骨架与 BOSS Mesh 一致。
# 骨架不一致 → Montage_Play 静默失败 → 表现就是"没有动画"。这是必须核实的点。
import unreal

OUT = "K:/selfgame/demo/Tools/_walk_montage_check.txt"
L = []
def log(s): L.append(str(s))

NAMES = ["Boss_Idle_Montage", "Boss_Walk_Fwd_Montage", "Boss_Walk_Bwd_Montage",
         "Boss_Walk_Left_Montage", "Boss_Walk_Right_Montage"]

for n in NAMES:
    p = f"/Game/Boss/Animation/{n}.{n}"
    m = unreal.load_object(None, p)
    if m is None:
        log(f"[MISS] {n}  → 没生成")
        continue
    sk = m.get_editor_property("target_skeleton")
    slots = []
    try:
        for st in m.get_editor_property("slot_anim_tracks"):
            try:
                slots.append(st.get_editor_property("slot_name"))
            except Exception:
                slots.append("?")
    except Exception as e:
        slots = [f"err:{e}"]
    ln = None
    try:
        ln = m.get_editor_property("loop")
    except Exception:
        pass
    src = None
    try:
        for st in m.get_editor_property("slot_anim_tracks"):
            src = st.get_editor_property("source_anim")
            break
    except Exception:
        pass
    log(f"[OK]   {n}")
    log(f"         骨架={sk.get_name() if sk else 'None'}")
    log(f"         Slot={slots}")
    log(f"         源动画={src.get_name() if src else 'None'}")
    log(f"         时长={float(m.get_editor_property('length')) if hasattr(m,'get_editor_property') else '?'}  循环属性={ln}")

# 顺带看一眼 BOSS Mesh 的骨架（从 CDO 读）
log("")
try:
    bp = unreal.load_object(None, "/Game/FightBoss/BP_Boss.BP_Boss")
    if bp:
        gc = bp.get_editor_property("generated_class") if bp else None
        log(f"BP_Boss = {bp.get_name()}, generated_class = {gc}")
except Exception as e:
    log(f"BP_Boss 读取失败: {e}")

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(L))
print("wrote", OUT)
