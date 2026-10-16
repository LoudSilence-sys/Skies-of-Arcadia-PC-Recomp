#!/usr/bin/env python3
"""framecmp.py REFDIR OURDIR [window] - match each Dolphin frame to the closest recomp frame (by index
window), report the worst matches. Frames are downscaled to 160x120 grayscale+color for robustness."""
import sys, os, re, numpy as np
from PIL import Image
ref, our = sys.argv[1], sys.argv[2]; W = int(sys.argv[3]) if len(sys.argv) > 3 else 300
def load(p):
    im = Image.open(p).convert('RGB').resize((160, 120), Image.BILINEAR); return np.asarray(im, np.float32)
R = {int(re.findall(r'\d+', f)[-1]): os.path.join(ref, f) for f in os.listdir(ref)}
O = {int(re.findall(r'\d+', f)[-1]): os.path.join(our, f) for f in os.listdir(our)}
oc = {}
def og(k):
    if k not in oc: oc[k] = load(O[k])
    return oc[k]
res = []
ok = sorted(O)
for k in sorted(R):
    r = load(R[k])
    if r.mean() < 3: continue
    best = (1e9, None)
    for j in ok:
        if abs(j - k) > W: continue
        d = np.abs(og(j) - r).mean()
        if d < best[0]: best = (d, j)
    res.append((best[0], k, best[1]))
res.sort(reverse=True)
for d, k, j in res[:int(sys.argv[4]) if len(sys.argv) > 4 else 25]: print(f'{d:6.1f}  ref {k}  ours {j}')
print('median', np.median([r[0] for r in res]))
