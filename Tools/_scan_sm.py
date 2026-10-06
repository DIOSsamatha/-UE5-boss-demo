import re

p = r"K:\selfgame\demo\Content\ParagonKhaimera\Characters\Heroes\Khaimera\Meshes\Khaimera.uasset"
data = open(p, 'rb').read()
i = data.find(b'SM_')
while i != -1:
    seg = data[i:i+60]
    print(i, repr(seg))
    i = data.find(b'SM_', i+1)
    if i > 0 and i > 5_000_000:
        break
