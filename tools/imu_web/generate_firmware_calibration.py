import json
from pathlib import Path

root = Path(__file__).resolve().parents[2]
rows = [json.loads((root / f'diagnostics/calibration/imu{i}_coefficients.json').read_text()) for i in range(3)]
text = '#pragma once\n#define IMU_CALIBRATION_VERSION "left-012-20261110-v1"\n'
for name, key in [('accel_bias', 'accel_bias_raw'), ('accel_sensitivity', 'accel_sensitivity_counts_per_g'), ('gyro_bias', 'gyro_bias_raw')]:
    text += f'static const float s_cal_{name}[3][3] = {{\n'
    for row in rows:
        text += '    {' + ', '.join(f'{v:.8f}f' for v in row[key]) + '},\n'
    text += '};\n'
(root / 'main/imu_calibration_coefficients.h').write_text(text)
print('Generated versioned firmware calibration for channels 0, 1, 2.')
