import re, os

paths = [
    r"K:\selfgame\demo\Content\ParagonKhaimera\Characters\Heroes\Khaimera\Animations\Idle_Relaxed_A.uasset",
    r"K:\selfgame\demo\Content\ParagonKhaimera\Characters\Heroes\Khaimera\Animations\Jog_Fwd.uasset",
]
for p in paths:
    print("==== " + os.path.basename(p) + " ====")
    if not os.path.exists(p):
        print("  MISSING")
        continue
    data = open(p, 'rb').read()
    for kw in [b'AAT_', b'Additive', b'bIsAdditive', b'AdditiveAnimType', b'RefPoseSeq',
               b'bEnableRootMotion', b'Interpolation', b'NumFrames', b'SequenceLength']:
        idxs = [m.start() for m in re.finditer(re.escape(kw), data)]
        if idxs:
            snippets = []
            for i in idxs[:3]:
                s = data[max(0, i-2):i+40]
                snippets.append(repr(s))
            print("  " + kw.decode() + " x" + str(len(idxs)) + " -> " + " | ".join(snippets))
