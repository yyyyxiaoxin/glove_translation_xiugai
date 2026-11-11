import concurrent.futures
from pathlib import Path
import serial
import time

def capture(name):
    port=serial.Serial()
    port.port=name;port.baudrate=115200;port.timeout=.2
    port.dtr=False;port.rts=False
    data=bytearray()
    with port:
        end=time.monotonic()+22
        while time.monotonic()<end:
            data.extend(port.read(port.in_waiting or 1))
    Path(f'diagnostics/connection_{name}.log').write_bytes(data)
    lines=data.decode('utf-8',errors='replace').splitlines()
    print(name, '\n'.join(line for line in lines if not line.startswith('[BAT]'))[-14000:])

with concurrent.futures.ThreadPoolExecutor() as pool:
    list(pool.map(capture,['COM14','COM17']))
