#pragma once
#include <stdbool.h>
#include "imu_acquisition.h"

/* output: acceleration m/s^2, angular velocity deg/s; raw frame is unchanged. */
bool imu_calibration_apply(int channel, const imu_channel_data_t *raw, float output[6]);
const char *imu_calibration_version(void);
