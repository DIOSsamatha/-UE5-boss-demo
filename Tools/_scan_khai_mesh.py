import re, os

p = r"K:\selfgame\demo\Content\ParagonKhaimera\Characters\Heroes\Khaimera\Meshes\Khaimera.uasset"
data = open(p, 'rb').read()
strs = [s.decode('latin-1') for s in re.findall(rb'[ -~]{4,}', data)]
seen = set(); out = []
for t in strs:
    if any(k in t for k in ['SKM_', 'SM_', 'SK_', 'Orb', 'Sphere', 'Weapon', 'Sword',
                            'Mesh', 'Material', 'M_Khaimera', 'Totem', 'Skull', 'Helmet']):
        if t not in seen:
            seen.add(t); out.append(t)
with open(r"K:\selfgame\demo\_khaimesh.txt", 'w', encoding='utf-8') as f:
    f.write("\n".join(out) + "\n")

# also list all animation names in the pack + additive status
anim_dir = r"K:\selfgame\demo\Content\ParagonKhaimera\Characters\Heroes\Khaimera\Animations"
rows = []
if os.path.isdir(anim_dir):
    for fn in sorted(os.listdir(anim_dir)):
        if not fn.endswith('.uasset'):
            continue
        d = open(os.path.join(anim_dir, fn), 'rb').read()
        additive = b'AAT_LocalSpaceBase' in d or b'AAT_RotationOffsetMeshSpace' in d
        none_ = b'AAT_None' in d
        rows.append(f"{fn}  additive={additive}  hasAAT_None={none_}")
with open(r"K:\selfgame\demo\_khai_anims.txt", 'w', encoding='utf-8') as f:
    f.write("\n".join(rows) + "\n")
print("done", len(out), len(rows))
