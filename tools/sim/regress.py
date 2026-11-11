"""Regressionstest: jeder Prototyp-Bildschirm wird über die echte App-Logik angesteuert und verglichen.

Aufruf: python regress.py [--device] [name ...]
  --device: statt Simulator Schnappschüsse vom Gerät holen (langsamer, Licht bleibt aus)
Ausgabe je Bildschirm: mittlere Abweichung, Anteil stark abweichender Pixel, Farbstich (Grün gegen Rot/Blau).
"""
import os
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(HERE, '../design/out')
PY = sys.executable

# Referenz -> Aktionen (wie ein Mensch tippen würde)
CASES = {
    'home': [], 'home_paused': ['state:paused'], 'home_finished': ['state:finished'], 'home_idle': ['state:idle'],
    'home_offline': ['state:offline'], 'home_error': ['state:error'], 'home_heating': ['state:heating'],
    'home_levelling': ['state:levelling'], 'home_lightoff': ['toggle:light', 'wait:3300'],
    'menu': ['nav:menu'], 'control': ['nav:control'], 'control_paused': ['state:paused', 'nav:control'],
    'temperatures': ['nav:temperatures'], 'filament': ['nav:filament'], 'filament_noams': ['toggle:ams', 'nav:filament'],
    'slot': ['slot:0'], 'slot_ext': ['slot:4'], 'wizard': ['state:idle', 'slot:0', 'wizard:laden'],
    'wizard_2': ['state:idle', 'slot:0', 'wizard:laden', 'wizard-next', 'wizard-next', 'wait:1500'],
    'files': ['nav:files'], 'files_usb': ['nav:files', 'source:USB-Stick'], 'files_empty': ['toggle:empty', 'nav:files'],
    'files_usb_empty': ['toggle:usb', 'nav:files', 'source:USB-Stick'], 'file': ['file:0'], 'file_1': ['file:1'],
    'movement': ['nav:movement'], 'fans': ['nav:fans'], 'history': ['nav:history'], 'messages': ['nav:messages'],
    'messages_error': ['state:error', 'nav:messages'], 'connection': ['nav:connection'], 'setup': ['nav:setup'],
    'power': ['nav:power'], 'system': ['nav:system'], 'demo': ['nav:demo'],
    'modal_cancel': ['nav:control', 'cancel'], 'modal_factory': ['nav:system', 'factory'],
    'modal_restart': ['nav:system', 'restart'], 'busy_homeaxes': ['state:idle', 'nav:movement', 'home-axes'],
    'sleep': ['sleep'], 'press_menu': ['press:nav:menu'],
    'scroll_system': ['nav:system', 'scroll:bottom'], 'scroll_demo': ['nav:demo', 'scroll:bottom'],
    'scroll_connection': ['nav:connection', 'scroll:bottom'], 'scroll_files': ['nav:files', 'scroll:bottom'],
    'scroll_movement': ['nav:movement', 'scroll:bottom'],
}


def to565(a):
    a = a.astype(np.float64)
    r = np.round(a[..., 0] * 31 / 255).astype(np.int32)
    g = np.round(a[..., 1] * 63 / 255).astype(np.int32)
    b = np.round(a[..., 2] * 31 / 255).astype(np.int32)
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.int32)


def main():
    args = sys.argv[1:]
    device = '--device' in args
    names = [a for a in args if not a.startswith('--')] or list(CASES)
    rows = []
    for n in names:
        out = os.path.join(HERE, f'r_{n}.png' if device else f'r_{n}.ppm')
        if device:
            subprocess.run([PY, '-I', os.path.join(HERE, 'device_snap.py'), out, 'reset'] + CASES[n],
                           check=True, capture_output=True)
        else:
            subprocess.run([os.path.join(HERE, 'sim'), out] + CASES[n], check=True, capture_output=True)
        ref = to565(np.array(Image.open(os.path.join(REF, n + '.png')).convert('RGB')))
        sim = np.array(Image.open(out).convert('RGB')).astype(np.int32)
        d = np.abs(ref - sim).max(-1)
        cast = ((sim[..., 1] - (sim[..., 0] + sim[..., 2]) / 2) - (ref[..., 1] - (ref[..., 0] + ref[..., 2]) / 2))
        rows.append((n, d.mean(), (d > 48).mean() * 100, cast.mean()))
        print(f'{n:16} mittel {d.mean():5.2f}   >48: {(d > 48).mean() * 100:4.2f}%   Farbstich {cast.mean():+5.2f}')
    m = np.mean([r[1] for r in rows])
    print(f'{"GESAMT":16} mittel {m:5.2f}   schlechtester: {max(rows, key=lambda r: r[1])[0]}')


if __name__ == '__main__':
    main()
