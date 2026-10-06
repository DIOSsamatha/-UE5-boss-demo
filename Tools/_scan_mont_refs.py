import os, re

d = r"K:\selfgame\demo\Content\Boss\Animation"
anim_dir = r"K:\selfgame\demo\Content\ParagonKhaimera\Characters\Heroes\Khaimera\Animations"
additive = {}
for fn in os.listdir(anim_dir):
    if fn.endswith('.uasset'):
        data = open(os.path.join(anim_dir, fn), 'rb').read()
        additive[fn[:-7]] = (b'AAT_LocalSpaceBase' in data or b'AAT_RotationOffsetMeshSpace' in data)

rows = []
for fn in sorted(os.listdir(d)):
    if not fn.endswith('.uasset'):
        continue
    data = open(os.path.join(d, fn), 'rb').read()
    names = set()
    for m in re.finditer(rb'Animations/([A-Za-z0-9_]+)\.[A-Za-z0-9_]+', data):
        names.add(m.group(1).decode('latin-1'))
    for m in re.finditer(rb'([A-Za-z0-9_]+)\.[A-Za-z0-9_]+', data):
        pass
    srcs = sorted(n for n in names if n in additive)
    tag = ", ".join("%s%s" % (n, "(ADD)" if additive[n] else "") for n in srcs) or "?"
    rows.append("%-40s <- %s" % (fn, tag))
with open(r"K:\selfgame\demo\_montrefs.txt", 'w', encoding='utf-8') as f:
    f.write("\n".join(rows) + "\n")
print("done", len(rows))
