import re

p = r"K:\selfgame\demo\Content\FightGround.umap"
data = open(p, 'rb').read()
strs = re.findall(rb'[ -~]{4,}', data)
texts = [s.decode('latin-1') for s in strs]

kws = ['Sphere', 'Orb', 'Ball', 'BasicShapes', 'Projectile', 'Impact', 'Burst',
       'Trail', 'Boss', 'Khaimera', 'StaticMesh', 'Weapon', 'Sword', 'Aura',
       'Target', 'Focus', 'Marker', 'Indicator', 'Light']
seen = set()
out = []
for t in texts:
    for k in kws:
        if k.lower() in t.lower():
            if t not in seen:
                seen.add(t); out.append(t)
            break

with open(r"K:\selfgame\demo\_scmap.txt", 'w', encoding='utf-8') as f:
    f.write("\n".join(out) + "\n")
print("done", len(out))
