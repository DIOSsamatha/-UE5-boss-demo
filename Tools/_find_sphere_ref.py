import os, re

roots = [
    r"K:\selfgame\demo\Content\Boss",
    r"K:\selfgame\demo\Content\FightPlayer",
    r"K:\selfgame\demo\Content\Game",
    r"K:\selfgame\demo\Content\UI",
    r"K:\selfgame\demo\Content\Combat",
    r"K:\selfgame\demo\Content",
]
roots = list(dict.fromkeys(roots))  # dedupe, keep order

pat = re.compile(rb'BasicShapes/Sphere|SM_Sphere|SM_Impact_Sphere|Sphere\.Sphere')
skip_pref = ('ParagonKhaimera', 'SwordAnimsetPro', 'MedievalSword')

hits = []
seen_files = set()
for root in roots:
    for dirpath, dirnames, filenames in os.walk(root):
        if any(s in dirpath for s in skip_pref):
            dirnames[:] = []
            continue
        for fn in filenames:
            if not fn.endswith('.uasset'):
                continue
            full = os.path.join(dirpath, fn)
            if full in seen_files:
                continue
            seen_files.add(full)
            try:
                if os.path.getsize(full) > 20 * 1024 * 1024:
                    continue
                data = open(full, 'rb').read()
            except Exception:
                continue
            if pat.search(data):
                found = sorted(set(m.group(0).decode('latin-1') for m in pat.finditer(data)))
                hits.append(full + "  ->  " + ",".join(found))

with open(r"K:\selfgame\demo\_sphereref.txt", 'w', encoding='utf-8') as f:
    f.write("\n".join(hits) + "\n")
print("files scanned:", len(seen_files), "hits:", len(hits))
