#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMU_MPU6050_COUNT   10
#define IMU_TOTAL_CHANNELS  11
#define IMU_ICM20948_INDEX  10

typedef struct {
    int16_t ax;
    int16_t ay;
    int16_t az;
    int16_t gx;
    int16_t gy;
    int16_t gz;
    int16_t mx;
    int16_t my;
    int16_t mz;
    bool valid;
    bool has_mag;
} imu_channel_data_t;

typedef struct {
    uint32_t sequence;
    int64_t timestamp_ms;
    imu_channel_data_t channel[IMU_TOTAL_CHANNELS];
} imu_frame_t;

esp_err_t imu_acquisition_init(void);
esp_err_t imu_acquisition_read_frame(imu_frame_t *frame);

/* 原始数据与经过零偏校正、低通滤波的数据接口。 */
esp_err_t imu_get_channel_raw_data(int channel, imu_channel_data_t *out);
esp_err_t imu_get_channel_data(int channel, imu_channel_data_t *out);

unsigned int imu_get_detected_count(void);

#ifdef __cplusplus
}
#endif
