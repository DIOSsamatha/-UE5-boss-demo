import unreal

def log(msg):
    with open("K:/selfgame/demo/Tools/_measure_root.txt", "a", encoding="utf-8") as f:
        f.write(msg + "\n")
        f.flush()

def main():
    try:
        log("=== MeasureJogRootBone v2 ===")
        asset = unreal.load_asset("/Game/ParagonKhaimera/Characters/Heroes/Khaimera/Animations/Jog_Fwd")
        skel = asset.get_skeleton()
        log("skel=%s" % (skel.get_name() if skel else "None"))
        ref = None
        try:
            ref = skel.get_reference_skeleton()
            log("get_reference_skeleton OK")
        except Exception as e:
            log("get_reference_skeleton err: %r" % e)
            try:
                ref = skel.reference_skeleton
                log("skel.reference_skeleton OK")
            except Exception as e2:
                log("skel.reference_skeleton err: %r" % e2)
        if ref is not None:
            try:
                names = ref.get_bone_names()
                log("bone count=%d" % len(names))
                for n in names[:20]:
                    log("  bone: %s" % n)
                for n in names:
                    if "root" in str(n).lower():
                        log("  >>> ROOTLIKE: %s" % n)
            except Exception as e:
                log("get_bone_names err: %r" % e)
        # also try get_bone_pose with "Root" and log real exception
        try:
            pose = asset.get_anim_pose_at_time(0.0, unreal.AnimPoseEvaluationOptions())
            tr = pose.get_bone_pose("Root", unreal.AnimPoseBoneEvaluationSpace.COMPONENT_SPACE)
            log("get_bone_pose('Root') OK loc=(%s)" % tr.translation)
        except Exception as e:
            log("get_bone_pose('Root') err: %r" % e)
        log("=== done ===")
    except Exception as e:
        log("FATAL: %r" % e)

main()
