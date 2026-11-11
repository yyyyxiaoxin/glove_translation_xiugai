import concurrent.futures
from pathlib import Path
import serial
import time

def capture(port):
    data = bytearray()
    with serial.Serial(port, 115200, timeout=.2) as connection:
        connection.dtr = False
        connection.rts = True
        time.sleep(.15)
        connection.rts = False
        end = time.monotonic() + 25
        while time.monotonic() < end:
            data.extend(connection.read(connection.in_waiting or 1))
    Path(f'diagnostics/update_{port}_boot.log').write_bytes(data)
    lines = data.decode('utf-8', errors='replace').splitlines()
    keys = ('HAND=', '检测到', 'MPU', 'ICM', 'IP', 'SNTP', 'Heartbeat', 'Error', 'error', 'panic', 'GPIO4', 'Failed', 'fail')
    return port, '\n'.join(line for line in lines if any(key in line for key in keys))[-10000:]

with concurrent.futures.ThreadPoolExecutor() as pool:
    for port, output in pool.map(capture, ['COM14', 'COM17']):
        print(port, output)
