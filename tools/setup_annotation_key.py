import secrets
from pathlib import Path
p=Path('build/imu.env')
s=p.read_text(encoding='utf-8')
if 'IMU_ANNOTATION_TOKEN=' not in s:
    s+='\nIMU_ANNOTATION_TOKEN=imu-label-'+secrets.token_hex(4)+'\n'
    p.write_text(s,encoding='utf-8')
print(next(line for line in s.splitlines() if line.startswith('IMU_ANNOTATION_TOKEN=')).split('=',1)[1])
