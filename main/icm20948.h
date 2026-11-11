#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ICM20948_DEFAULT_ADDRESS 0x68
#define ICM20948_WHO_AM_I_VALUE  0xEA
#define AK09916_DEFAULT_ADDRESS  0x0C

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
    bool mag_valid;
} icm20948_raw_data_t;

typedef struct {
    i2c_master_dev_handle_t device;
    i2c_master_dev_handle_t magnetometer;
    bool mag_present;
    bool last_mag_valid;
    int16_t last_mx;
    int16_t last_my;
    int16_t last_mz;
} icm20948_handle_t;

esp_err_t icm20948_create(icm20948_handle_t *handle,
                          i2c_master_bus_handle_t bus_handle,
                          uint32_t scl_speed_hz);

/* 初始化当前已选中的TCA9548A下游通道。 */
esp_err_t icm20948_configure_selected(icm20948_handle_t *handle);

esp_err_t icm20948_read_raw(icm20948_handle_t *handle,
                            icm20948_raw_data_t *data);

#ifdef __cplusplus
}
#endif
