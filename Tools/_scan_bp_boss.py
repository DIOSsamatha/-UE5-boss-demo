import re, sys

p = r"K:\selfgame\demo\Content\Boss\System\BP_Boss.uasset"
data = open(p, 'rb').read()
strs = re.findall(rb'[ -~]{4,}', data)
texts = [s.decode('latin-1') for s in strs]

kws = ['Sphere', 'Orb', 'Ball', 'BasicShapes', 'StaticMesh', 'SkeletalMesh',
       'Khaimera', 'Weapon', 'Sword', 'Mesh', 'Light', 'Widget', 'Trail',
       'Projectile', 'Impact', 'Aura', 'Shield', 'Helmet', 'Head']
seen = set()
out = []
for t in texts:
    for k in kws:
        if k.lower() in t.lower():
            if t not in seen:
                seen.add(t)
                out.append(t)
            break

with open(r"K:\selfgame\demo\_scboss.txt", 'w', encoding='utf-8') as f:
    for t in out:
        f.write(t + "\n")
print("done", len(out))
