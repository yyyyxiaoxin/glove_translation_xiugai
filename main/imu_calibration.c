#include "imu_calibration.h"
#include "imu_calibration_coefficients.h"
#include "sdkconfig.h"

const char *imu_calibration_version(void)
{
#if CONFIG_GLOVE_HAND_LEFT
    return IMU_CALIBRATION_VERSION;
#else
    return "none";
#endif
}

bool imu_calibration_apply(int channel, const imu_channel_data_t *raw, float output[6])
{
#if CONFIG_GLOVE_HAND_LEFT
    if (channel < 0 || channel >= 3 || !raw || !raw->valid || !output) return false;
    const float a[3] = {raw->ax, raw->ay, raw->az};
    const float g[3] = {raw->gx, raw->gy, raw->gz};
    for (int axis = 0; axis < 3; ++axis) {
        output[axis] = (a[axis] - s_cal_accel_bias[channel][axis]) /
                      s_cal_accel_sensitivity[channel][axis] * 9.80665f;
        output[axis + 3] = (g[axis] - s_cal_gyro_bias[channel][axis]) / 16.4f;
    }
    return true;
#else
    return false;
#endif
}
