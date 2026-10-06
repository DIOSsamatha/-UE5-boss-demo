import os

d = r"K:\selfgame\demo\Content\Boss\Animation"
rows = []
for fn in sorted(os.listdir(d)):
    if not fn.endswith('.uasset'):
        continue
    data = open(os.path.join(d, fn), 'rb').read()
    additive = (b'AAT_LocalSpaceBase' in data) or (b'AAT_RotationOffsetMeshSpace' in data)
    rows.append(f"{fn:44s} additive={additive}")
with open(r"K:\selfgame\demo\_bossmont.txt", 'w', encoding='utf-8') as f:
    f.write("\n".join(rows) + "\n")
print("done", len(rows))
