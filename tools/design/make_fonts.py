"""Erzeugt die LVGL-Schriften für Soft White aus Manrope (variabel) in exakten Stärken.

tnum=1: Ziffern auf die Tabellenziffern (.tf) umgelegt, wie `font-variant-numeric: tabular-nums`.
Aufruf: python make_fonts.py <ausgabe-ordner> <temp-ordner> <pfad zu lv_font_conv>
"""
import os
import subprocess
import sys

from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../design/soft-white/Manrope-VF.ttf')
OUT, TMP, CONV = sys.argv[1], sys.argv[2], sys.argv[3]

# (Größe px, Stärke, Tabellenziffern) – aus den gemessenen Textzeilen aller Bildschirme
FONTS = [(18, 400, 0), (18, 700, 0), (19, 400, 0), (20, 400, 0), (20, 600, 0), (21, 700, 0),
         (23, 650, 0), (24, 400, 0), (25, 600, 0), (26, 600, 0), (28, 400, 0), (28, 650, 0),
         (28, 700, 0), (30, 700, 0), (31, 600, 0), (40, 600, 0), (42, 600, 1), (48, 650, 1),
         (58, 650, 1)]
RANGES = '0x20-0x7E,0xA0-0xFF,0x2013,0x2022,0x2026,0x2192,0x2212'
DIGITS = ['zero', 'one', 'two', 'three', 'four', 'five', 'six', 'seven', 'eight', 'nine']

os.makedirs(OUT, exist_ok=True)
os.makedirs(TMP, exist_ok=True)
for size, wght, tnum in FONTS:
    ttf = os.path.join(TMP, f'Manrope-{wght}{"-tnum" if tnum else ""}.ttf')
    if not os.path.exists(ttf):
        f = instancer.instantiateVariableFont(TTFont(SRC), {'wght': wght})
        if tnum:
            for t in f['cmap'].tables:
                for d, n in zip('0123456789', DIGITS):
                    if ord(d) in t.cmap:
                        t.cmap[ord(d)] = n + '.tf'
        f.save(ttf)
    name = f'sw_font_{size}_{wght}' + ('_tnum' if tnum else '')
    out = os.path.join(OUT, name + '.c')
    subprocess.run([CONV, '--size', str(size), '--bpp', '4', '--no-compress', '--font', ttf,
                    '-r', RANGES, '--format', 'lvgl', '--autohint-off',
                    '--lv-include', 'lvgl.h', '-o', out], check=True, capture_output=True)
    print(name, os.path.getsize(out) // 1024, 'KB Quelltext')
