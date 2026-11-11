"""Fit an initial diagonal six-face calibration; never overwrite raw records."""
import json
import math
from pathlib import Path
import statistics as st
import sys

ch = int(sys.argv[1])
folder = Path(__file__).resolve().parents[2] / 'diagnostics/calibration'
poses = {}
for path in sorted(folder.glob(f'imu{ch}_*.json')):
    data = json.loads(path.read_text(encoding='utf-8'))
    if data.get('accepted') and data.get('channel') == ch:
        poses[data['pose']] = data
if set(poses) != {'+X', '-X', '+Y', '-Y', '+Z', '-Z'}:
    raise SystemExit('Six accepted poses required')
bias, sensitivity = [], []
for axis, key in zip('XYZ', ('ax', 'ay', 'az')):
    plus = poses['+' + axis]['mean_raw'][key]
    minus = poses['-' + axis]['mean_raw'][key]
    bias.append((plus + minus) / 2)
    sensitivity.append((plus - minus) / 2)
# Median across pose means reduces the effect of individual orientation-dependent
# bias or movement. This does not calibrate gyro gain or cross-axis response.
gyro = [st.median(p['mean_raw'][key] for p in poses.values()) for key in ('gx', 'gy', 'gz')]
validation = {}
for pose, data in poses.items():
    corrected = [(data['mean_raw'][key] - bias[i]) / sensitivity[i] * 9.80665
                 for i, key in enumerate(('ax', 'ay', 'az'))]
    validation[pose] = {'acceleration_ms2': corrected,
                        'norm_ms2': math.sqrt(sum(v*v for v in corrected))}
result = {'channel': ch, 'status': 'initial_six_face_fit_not_applied',
          'accel_bias_raw': bias, 'accel_sensitivity_counts_per_g': sensitivity,
          'gyro_bias_raw': gyro, 'gyro_bias_dps': [x/16.4 for x in gyro],
          'pose_validation': validation,
          'samples': sum(p['samples'] for p in poses.values()),
          'limitations': 'Manual alignment; diagonal fit only. Independent pose validation required. Gyro gain not calibrated.',
          'source_recorded_at': {pose: data['recorded_at'] for pose, data in poses.items()}}
out = folder / f'imu{ch}_coefficients.json'
out.write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(result, indent=2))
