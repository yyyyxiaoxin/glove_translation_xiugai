#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MPU6050_DEFAULT_ADDRESS 0x68

typedef struct {
    int16_t ax;
    int16_t ay;
    int16_t az;
    int16_t temperature;
    int16_t gx;
    int16_t gy;
    int16_t gz;
} mpu6050_raw_data_t;

typedef struct {
    float accel_x_g;
    float accel_y_g;
    float accel_z_g;
    float gyro_x_dps;
    float gyro_y_dps;
    float gyro_z_dps;
    float temperature_c;
} mpu6050_data_t;

typedef struct {
    i2c_master_dev_handle_t device;
    uint8_t who_am_i;
} mpu6050_handle_t;

/* 每条I2C总线只创建一个0x68设备句柄，切换TCA通道后复用该句柄。 */
esp_err_t mpu6050_create(mpu6050_handle_t *handle,
                         i2c_master_bus_handle_t bus_handle,
                         uint8_t address,
                         uint32_t scl_speed_hz);

/* 初始化当前已选中的TCA9548A下游通道。 */
esp_err_t mpu6050_configure_selected(mpu6050_handle_t *handle,
                                     uint8_t *who_am_i);

esp_err_t mpu6050_read_raw(mpu6050_handle_t *handle,
                           mpu6050_raw_data_t *data);

/* 保留单模块测试用的物理量读取接口。 */
esp_err_t mpu6050_read(mpu6050_handle_t *handle, mpu6050_data_t *data);

#ifdef __cplusplus
}
#endif
