import os, re
d = r"K:\selfgame\demo\Content\Boss\Animation"
for fn in ["Boss_Hit_F_Montage.uasset", "Boss_Charge_Dash_Montage.uasset", "Boss_Slash_01_Montage.uasset"]:
    data = open(os.path.join(d, fn), 'rb').read()
    strs = sorted(set(s.decode('latin-1') for s in re.findall(rb'[ -~]{5,}', data)))
    keep = [s for s in strs if any(k in s for k in ('/Game/', 'Animations', 'Melee', 'HitReact', 'E_Ability', 'R_Ability', 'Death', 'Stunned', 'Knockback', 'Emote', 'Idle', 'Jog', 'Cast', 'DefaultSlot', 'Slot'))]
    print("==== " + fn + " ====")
    for s in keep:
        print("   " + s)
