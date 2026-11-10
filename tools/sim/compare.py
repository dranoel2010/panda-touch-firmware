"""Vergleicht Simulator-Bild mit der Chrome-Referenz (auf RGB565 gerundet, wie das Display).

Aufruf: python compare.py <referenz.png> <sim.ppm> <vergleich.png>
Gibt mittlere/maximale Abweichung aus und schreibt Referenz | Simulator | Differenz (×4).
"""
import sys

import numpy as np
from PIL import Image


def to565(a):
    a = a.astype(np.float64)
    r = np.round(a[..., 0] * 31 / 255).astype(np.int32)
    g = np.round(a[..., 1] * 63 / 255).astype(np.int32)
    b = np.round(a[..., 2] * 31 / 255).astype(np.int32)
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.uint8)


ref = to565(np.array(Image.open(sys.argv[1]).convert('RGB')))
sim = np.array(Image.open(sys.argv[2]).convert('RGB'))
d = np.abs(ref.astype(int) - sim.astype(int)).max(-1)
print(f'mittel {d.mean():.2f}  max {d.max()}  Pixel >16: {(d > 16).mean() * 100:.2f}%  >48: {(d > 48).mean() * 100:.2f}%')
heat = np.clip(d * 4, 0, 255).astype(np.uint8)
out = np.concatenate([ref, sim, np.stack([heat] * 3, -1)], 1)
Image.fromarray(out).save(sys.argv[3])
