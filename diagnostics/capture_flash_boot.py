import pathlib
import time
import sys

import serial

with serial.Serial('COM17', 115200, timeout=0.3) as port:
    if '--no-reset' not in sys.argv:
        port.dtr = False
        port.rts = True
        time.sleep(0.15)
        port.rts = False
    data = bytearray()
    end = time.monotonic() + 25
    while time.monotonic() < end:
        data.extend(port.read(port.in_waiting or 1))

path = pathlib.Path(__file__).with_name('flash_left_COM17_20261110.log')
path.write_bytes(data)
lines = data.decode('utf-8', errors='replace').splitlines()
print('Log:', path, 'bytes:', len(data))
print('\n'.join(line for line in lines if not line.startswith('[DATA]'))[:22000])
print('DATA_FRAMES:', sum(line.startswith('[DATA]') for line in lines))
