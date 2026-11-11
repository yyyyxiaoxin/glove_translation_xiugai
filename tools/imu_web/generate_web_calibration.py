import json
from pathlib import Path
root = Path(__file__).resolve().parents[2]
channels = {}
for ch in range(10):
    path = root / f'diagnostics/calibration/imu{ch}_coefficients.json'
    if path.exists():
        fit = json.loads(path.read_text(encoding='utf-8'))
        channels[str(ch)] = {key: fit[key] for key in ('accel_bias_raw', 'accel_sensitivity_counts_per_g', 'gyro_bias_raw', 'samples')}
Path(__file__).with_name('calibration_coefficients.json').write_text(json.dumps({'version': 'left-web-20261111-v1', 'channels': channels}, indent=2), encoding='utf-8')
print('Web calibration channels:', ', '.join(channels))
