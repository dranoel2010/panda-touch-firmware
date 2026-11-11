"""Dauertest auf dem Gerät: klickt sich wiederholt durch alle Bildschirme und prüft, ob Speicher verloren geht.
Aufruf: python device_soak.py [runden]"""
import glob, sys, time
import serial
s = serial.Serial()
s.port, s.baudrate, s.timeout = glob.glob('/dev/cu.wchusbserial*')[0], 115200, 1
s.dtr = s.rts = False
s.open()
time.sleep(0.2)
s.reset_input_buffer()
def cmd(c, until=b'\n'):
    s.write((c + '\n').encode())
    t0, b = time.time(), b''
    while not b.endswith(until):
        b += s.read(64)
        if time.time() - t0 > 5: break
    return b.decode(errors='replace').strip().splitlines()[-1] if b.strip() else ''
PFAD = ['nav:menu', 'nav:control', 'speed:Sport', 'cancel', 'modal-close', 'nav:temperatures', 'temp:nozzle:5',
        'nav:filament', 'slot:1', 'select:slot-material', 'nav:files', 'source:USB-Stick', 'file:1', 'nav:movement',
        'step:1', 'nav:fans', 'nav:history', 'nav:messages', 'nav:connection', 'select:wifi', 'nav:power',
        'nav:system', 'toggle:sound', 'toggle:sound', 'nav:demo', 'state:paused', 'state:printing', 'nav:home']
cmd('dark 1')
runden = int(sys.argv[1]) if len(sys.argv) > 1 else 5
print('Start   ', cmd('mem'))
for r in range(runden):
    for a in PFAD:
        cmd('go ' + a)
        time.sleep(0.6)
    time.sleep(1)
    print(f'Runde {r + 1}', cmd('mem'))
