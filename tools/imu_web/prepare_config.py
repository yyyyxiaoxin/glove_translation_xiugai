import json
from pathlib import Path
import secrets

root = Path(__file__).resolve().parents[2]
env_file = root / 'build/imu.env'
if env_file.exists():
    token = env_file.read_text().splitlines()[0].split('=', 1)[1]
else:
    token = secrets.token_hex(32)
    env_file.write_text(f'IMU_TOKEN={token}\nIMU_DB=/var/lib/glove-imu/imu.sqlite3\n')
(root / 'main/cloud_config.h').write_text(
    '#pragma once\n#define GLOVE_CLOUD_URL "https://yeyouzzz.cn/imu/api/ingest"\n'
    f'#define GLOVE_CLOUD_TOKEN {json.dumps(token)}\n')
print('Local cloud credentials prepared (token not printed).')
