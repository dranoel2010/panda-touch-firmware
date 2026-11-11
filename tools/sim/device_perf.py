"""Misst auf dem Gerät, wie lange die echte Display-Aktualisierung nach einer Aktion dauert.
Aufruf: python device_perf.py aktion [aktion ...]"""
import glob, sys, time
import serial
s = serial.Serial()
s.port, s.baudrate, s.timeout = glob.glob('/dev/cu.wchusbserial*')[0], 115200, 1
s.dtr = s.rts = False
s.open()
time.sleep(0.2)
s.reset_input_buffer()
def cmd(c):
    s.write((c + '\n').encode())
    t0, b = time.time(), b''
    while not b.endswith(b'\n') or (b'OK' not in b and b'PERF' not in b and b'BENCH' not in b):
        b += s.read(64)
        if time.time() - t0 > 5: break
    return b.decode(errors='replace').strip().splitlines()[-1]
cmd('dark 1')
for a in sys.argv[1:]:
    if a == 'bench':
        print(cmd('bench'))
        continue
    if a.startswith('prof:'):
        cmd('prof ' + a[5:])
        time.sleep(2.5)
        print(f'{a:24} letzte Aktualisierungen (ms): {cmd("perf")[5:]}')
        continue
    cmd('go ' + a)
    time.sleep(float(__import__("os").environ.get("PERF_WAIT","1.5")))
    print(f'{a:24} letzte Aktualisierungen (ms): {cmd("perf")[5:]}')
