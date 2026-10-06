import re, os

d = r"K:\selfgame\demo\Content\Boss\VFX"
out = []
for fn in sorted(os.listdir(d)):
    if not fn.endswith('.uasset'):
        continue
    p = os.path.join(d, fn)
    data = open(p, 'rb').read()
    strs = sorted(set(s.decode('latin-1') for s in re.findall(rb'[ -~]{4,}', data)))
    rel = [s for s in strs if s.startswith('/Game/') or s.startswith('/Engine/')
           or s.startswith('M_') or s.startswith('MI_') or s.startswith('T_') or s.startswith('Material')]
    out.append("==== " + fn + " ====")
    out.append("   " + " | ".join(rel))
with open(r"K:\selfgame\demo\_vfx.txt", 'w', encoding='utf-8') as f:
    f.write("\n".join(out) + "\n")
print("done")
