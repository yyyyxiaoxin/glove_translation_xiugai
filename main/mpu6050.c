#include "mpu6050.h"

#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MPU6050_REG_SMPLRT_DIV     0x19
#define MPU6050_REG_CONFIG         0x1A
#define MPU6050_REG_GYRO_CONFIG    0x1B
#define MPU6050_REG_ACCEL_CONFIG   0x1C
#define MPU6050_REG_FIFO_EN        0x23
#define MPU6050_REG_INT_PIN_CFG    0x37
#define MPU6050_REG_INT_ENABLE     0x38
#define MPU6050_REG_ACCEL_XOUT_H   0x3B
#define MPU6050_REG_USER_CTRL      0x6A
#define MPU6050_REG_PWR_MGMT_1     0x6B
#define MPU6050_REG_PWR_MGMT_2     0x6C
#define MPU6050_REG_WHO_AM_I       0x75

#define MPU6050_TIMEOUT_MS         100

/* 与原工程保持一致：50 Hz、加速度±2 g、陀螺仪±2000 dps。 */
#define MPU6050_SAMPLE_DIV_100HZ   9
#define MPU6050_DLPF_21HZ          4
#define MPU6050_ACCEL_FS_2G        0x00
#define MPU6050_GYRO_FS_2000DPS    0x18

static const char *TAG = "mpu6050";

static esp_err_t write_register(mpu6050_handle_t *handle,
                                uint8_t reg, uint8_t value)
{
    const uint8_t packet[] = {reg, value};
    return i2c_master_transmit(handle->device, packet, sizeof(packet),
                               MPU6050_TIMEOUT_MS);
}

static esp_err_t read_registers(mpu6050_handle_t *handle, uint8_t reg,
                                uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(handle->device, &reg, 1, data, length,
                                       MPU6050_TIMEOUT_MS);
}

static int16_t read_be16(const uint8_t *data)
{
    return (int16_t)(((uint16_t)data[0] << 8) | data[1]);
}

esp_err_t mpu6050_create(mpu6050_handle_t *handle,
                         i2c_master_bus_handle_t bus_handle,
                         uint8_t address,
                         uint32_t scl_speed_hz)
{
    ESP_RETURN_ON_FALSE(handle != NULL && bus_handle != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "参数无效");

    memset(handle, 0, sizeof(*handle));
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = scl_speed_hz,
    };
    return i2c_master_bus_add_device(bus_handle, &config, &handle->device);
}

esp_err_t mpu6050_configure_selected(mpu6050_handle_t *handle,
                                     uint8_t *who_am_i)
{
    ESP_RETURN_ON_FALSE(handle != NULL && handle->device != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "设备句柄无效");

    uint8_t who = 0;
    ESP_RETURN_ON_ERROR(read_registers(handle, MPU6050_REG_WHO_AM_I, &who, 1),
                        TAG, "读取WHO_AM_I失败");

    /* 0x68为MPU6050，0x70为当前实测兼容模块（通常是MPU6500）。 */
    ESP_RETURN_ON_FALSE(who == 0x68 || who == 0x70,
                        ESP_ERR_NOT_FOUND, TAG,
                        "不支持的WHO_AM_I: 0x%02X", who);

    ESP_RETURN_ON_ERROR(write_register(handle, MPU6050_REG_PWR_MGMT_1, 0x80),
                        TAG, "复位失败");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(write_register(handle, MPU6050_REG_PWR_MGMT_1, 0x00),
                        TAG, "唤醒失败");
    ESP_RETURN_ON_ERROR(write_register(handle, MPU6050_REG_GYRO_CONFIG,
                                       MPU6050_GYRO_FS_2000DPS),
                        TAG, "配置陀螺仪量程失败");
    ESP_RETURN_ON_ERROR(write_register(handle, MPU6050_REG_ACCEL_CONFIG,
                                       MPU6050_ACCEL_FS_2G),
                        TAG, "配置加速度量程失败");
    ESP_RETURN_ON_ERROR(write_register(handle, MPU6050_REG_SMPLRT_DIV,
                                       MPU6050_SAMPLE_DIV_100HZ),
                        TAG, "配置采样率失败");
    ESP_RETURN_ON_ERROR(write_register(handle, MPU6050_REG_CONFIG,
                                       MPU6050_DLPF_21HZ),
                        TAG, "配置低通滤波失败");
    ESP_RETURN_ON_ERROR(write_register(handle, MPU6050_REG_INT_ENABLE, 0x00),
                        TAG, "关闭中断失败");
    ESP_RETURN_ON_ERROR(write_register(handle, MPU6050_REG_USER_CTRL, 0x00),
                        TAG, "关闭I2C主模式失败");
    ESP_RETURN_ON_ERROR(write_register(handle, MPU6050_REG_FIFO_EN, 0x00),
                        TAG, "关闭FIFO失败");
    ESP_RETURN_ON_ERROR(write_register(handle, MPU6050_REG_INT_PIN_CFG, 0x80),
                        TAG, "配置中断引脚失败");
    ESP_RETURN_ON_ERROR(write_register(handle, MPU6050_REG_PWR_MGMT_1, 0x01),
                        TAG, "配置时钟源失败");
    ESP_RETURN_ON_ERROR(write_register(handle, MPU6050_REG_PWR_MGMT_2, 0x00),
                        TAG, "启用传感器轴失败");

    handle->who_am_i = who;
    if (who_am_i != NULL) {
        *who_am_i = who;
    }
    return ESP_OK;
}

esp_err_t mpu6050_read_raw(mpu6050_handle_t *handle,
                           mpu6050_raw_data_t *data)
{
    ESP_RETURN_ON_FALSE(handle != NULL && handle->device != NULL && data != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "参数无效");

    uint8_t raw[14];
    ESP_RETURN_ON_ERROR(read_registers(handle, MPU6050_REG_ACCEL_XOUT_H,
                                       raw, sizeof(raw)),
                        TAG, "读取采样数据失败");

    data->ax = read_be16(&raw[0]);
    data->ay = read_be16(&raw[2]);
    data->az = read_be16(&raw[4]);
    data->temperature = read_be16(&raw[6]);
    data->gx = read_be16(&raw[8]);
    data->gy = read_be16(&raw[10]);
    data->gz = read_be16(&raw[12]);
    return ESP_OK;
}

esp_err_t mpu6050_read(mpu6050_handle_t *handle, mpu6050_data_t *data)
{
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "参数无效");

    mpu6050_raw_data_t raw;
    ESP_RETURN_ON_ERROR(mpu6050_read_raw(handle, &raw), TAG, "读取失败");

    data->accel_x_g = raw.ax / 16384.0f;
    data->accel_y_g = raw.ay / 16384.0f;
    data->accel_z_g = raw.az / 16384.0f;
    data->temperature_c = raw.temperature / 340.0f + 36.53f;
    data->gyro_x_dps = raw.gx / 16.4f;
    data->gyro_y_dps = raw.gy / 16.4f;
    data->gyro_z_dps = raw.gz / 16.4f;
    return ESP_OK;
}
