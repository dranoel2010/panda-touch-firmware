"""Dauerlast: klickt N Runden durch die Bildschirme (mit Scroll-Animationen) und gibt die Display-Messung aus."""
import os, sys, time, serial
s = serial.Serial(); s.port, s.baudrate, s.timeout = os.environ.get('PORT', '/dev/cu.wchusbserial1240'), 115200, 0.3
s.dtr = s.rts = False; s.open(); time.sleep(0.2); s.reset_input_buffer()
def cmd(c, key=b'OK'):
    s.write((c+'\n').encode()); t0=time.time(); b=b''
    while time.time()-t0 < 4:
        b += s.read(256)
        if b.endswith(b'\n') and key in b: break
    for l in b.decode(errors='replace').splitlines():
        if key.decode() in l: return l
    return '?'
PFAD = ['nav:menu','nav:control','nav:temperatures','nav:filament','scroll:anim','nav:files','nav:movement','nav:fans',
        'nav:history','scroll:anim','nav:messages','nav:connection','nav:system','scroll:anim','nav:menu','nav:home']
n = int(sys.argv[1]) if len(sys.argv) > 1 else 5
pause = float(sys.argv[2]) if len(sys.argv) > 2 else 0.45
cmd('lcd', b'LCD')
for r in range(n):
    for a in PFAD:
        cmd('go ' + a); time.sleep(1.2 if a.startswith('scroll') else pause)
print(cmd('lcd', b'LCD'))
print(cmd('mem', b'MEM'))
