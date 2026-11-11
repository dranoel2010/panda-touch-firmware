"""Holt Bildschirm-Schnappschüsse vom Gerät über den Debug-Kanal (main/sw_debug.c).

Aufruf: python device_snap.py <ausgabe.png> [aktion ...]
Jede Aktion wird wie ein Antippen ausgelöst ("go <aktion>"), danach kommt der Schnappschuss.
Daten kommen als Base64-Zeilen mit CRC32; bei Fehlern wird wiederholt.
"""
import base64
import binascii
import glob
import struct
import sys
import time
import zlib

import numpy as np
import serial
from PIL import Image

port = glob.glob('/dev/cu.wchusbserial*')[0]
s = serial.Serial()          # Steuerleitungen vor dem Öffnen setzen, sonst startet das Gerät neu
s.port, s.baudrate, s.timeout = port, 115200, 1
s.dtr = False
s.rts = False
s.open()
time.sleep(0.2)
s.reset_input_buffer()


def cmd(c, wait=5):
    s.write((c + '\n').encode())
    t0, buf = time.time(), b''
    while b'OK' not in buf or not buf.endswith(b'\n'):
        buf += s.read(64)
        if time.time() - t0 > wait:
            break
    if b'OK ' in buf and c.startswith('go '):
        print(f'{c}: Aufbau {buf.split(b"OK ")[1].split()[0].decode()} ms')


cmd('dark 1')   # nachts: Licht bleibt aus, auch beim Aufwecken
for a in sys.argv[2:]:
    if a.startswith('wait:'):
        time.sleep(int(a[5:]) / 1000)
        continue
    cmd('go ' + a)
    time.sleep(0.4)

for attempt in range(4):
    time.sleep(0.3)
    s.reset_input_buffer()
    t0 = time.time()
    s.write(b'snap\n')
    lines, w = [], None
    while True:
        ln = s.readline()
        if not ln:
            if time.time() - t0 > 120:
                sys.exit('Zeitüberschreitung')
            continue
        ln = ln.strip()
        if ln.startswith(b'SNAP '):
            w, h = map(int, ln[5:].split())
            lines = []
        elif ln.startswith(b'D ') and w:
            lines.append(ln[2:])
        elif ln.startswith(b'SNAPEND') and w:
            parts = ln.split()
            total, crc = parts[1], parts[2]
            if len(parts) > 3:
                print(f'Zeichnen des ganzen Bildschirms: {parts[3].decode()} ms')
            break
    try:
        data = b''.join(base64.b64decode(x) for x in lines)
    except binascii.Error:
        print('Base64-Fehler, neuer Versuch')
        continue
    if len(data) != int(total) or (zlib.crc32(data) & 0xffffffff) != int(crc, 16):
        print(f'Prüfsumme falsch ({len(data)} / {int(total)}), neuer Versuch')
        continue
    px = np.empty(w * h, np.uint16)
    k = 0
    for cnt, p in struct.iter_unpack('<HH', data):
        px[k:k + cnt] = p
        k += cnt
    px = px.reshape(h, w)
    r = ((px >> 11) & 31).astype(np.uint8)
    g = ((px >> 5) & 63).astype(np.uint8)
    b = (px & 31).astype(np.uint8)
    Image.fromarray(np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1)).save(sys.argv[1])
    print(f'{w}x{h}, {len(data) // 1024} KB in {time.time() - t0:.1f} s, Prüfsumme ok')
    break
else:
    sys.exit('Schnappschuss fehlgeschlagen')
