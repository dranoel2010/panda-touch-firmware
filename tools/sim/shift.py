"""Misst je Bereich, ob Simulator und Referenz gegeneinander verschoben sind."""
import sys
import numpy as np
from PIL import Image
ref = np.array(Image.open(sys.argv[1]).convert('L')).astype(float)
sim = np.array(Image.open(sys.argv[2]).convert('L')).astype(float)
regs = [tuple(map(int, r.split(','))) for r in sys.argv[3:]]
for (x, y, w, h) in regs:
    best = None
    for dy in range(-3, 4):
        for dx in range(-3, 4):
            e = np.abs(ref[y:y+h, x:x+w] - sim[y+dy:y+dy+h, x+dx:x+dx+w]).mean()
            if best is None or e < best[0]: best = (e, dx, dy)
    e0 = np.abs(ref[y:y+h, x:x+w] - sim[y:y+h, x:x+w]).mean()
    print(f'{x},{y},{w},{h}: ohne {e0:5.2f}  bester dx={best[1]:+d} dy={best[2]:+d} -> {best[0]:5.2f}')
