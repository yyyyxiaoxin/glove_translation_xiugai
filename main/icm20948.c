#include "icm20948.h"

#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ICM_REG_WHO_AM_I        0x00
#define ICM_REG_USER_CTRL       0x03
#define ICM_REG_PWR_MGMT_1      0x06
#define ICM_REG_INT_PIN_CFG     0x0F
#define ICM_REG_ACCEL_XOUT_H    0x2D
#define ICM_REG_BANK_SEL        0x7F

#define ICM_BANK2_GYRO_CONFIG_1 0x01
#define ICM_BANK2_ACCEL_CONFIG  0x14

#define AK09916_REG_WIA2        0x01
#define AK09916_REG_ST1         0x10
#define AK09916_REG_HXL         0x11
#define AK09916_REG_CNTL2       0x31
#define AK09916_REG_CNTL3       0x32
#define AK09916_WHO_AM_I_VALUE  0x09

#define ICM_TIMEOUT_MS          100

static const char *TAG = "icm20948";

static esp_err_t device_write(i2c_master_dev_handle_t device,
                              uint8_t reg, uint8_t value)
{
    const uint8_t packet[] = {reg, value};
    return i2c_master_transmit(device, packet, sizeof(packet), ICM_TIMEOUT_MS);
}

static esp_err_t device_read(i2c_master_dev_handle_t device, uint8_t reg,
                             uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(device, &reg, 1, data, length,
                                       ICM_TIMEOUT_MS);
}

static esp_err_t select_bank(icm20948_handle_t *handle, uint8_t bank)
{
    return device_write(handle->device, ICM_REG_BANK_SEL,
                        (uint8_t)(bank << 4));
}

static int16_t read_be16(const uint8_t *data)
{
    return (int16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static int16_t read_le16(const uint8_t *data)
{
    return (int16_t)(((uint16_t)data[1] << 8) | data[0]);
}

esp_err_t icm20948_create(icm20948_handle_t *handle,
                          i2c_master_bus_handle_t bus_handle,
                          uint32_t scl_speed_hz)
{
    ESP_RETURN_ON_FALSE(handle != NULL && bus_handle != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "参数无效");
    memset(handle, 0, sizeof(*handle));

    const i2c_device_config_t icm_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ICM20948_DEFAULT_ADDRESS,
        .scl_speed_hz = scl_speed_hz,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus_handle, &icm_config,
                                                   &handle->device),
                        TAG, "创建ICM-20948设备句柄失败");

    const i2c_device_config_t mag_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AK09916_DEFAULT_ADDRESS,
        .scl_speed_hz = scl_speed_hz,
    };
    return i2c_master_bus_add_device(bus_handle, &mag_config,
                                     &handle->magnetometer);
}

esp_err_t icm20948_configure_selected(icm20948_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL && handle->device != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "设备句柄无效");

    ESP_RETURN_ON_ERROR(select_bank(handle, 0), TAG, "选择Bank 0失败");

    uint8_t who = 0;
    ESP_RETURN_ON_ERROR(device_read(handle->device, ICM_REG_WHO_AM_I,
                                    &who, 1),
                        TAG, "读取WHO_AM_I失败");
    ESP_RETURN_ON_FALSE(who == ICM20948_WHO_AM_I_VALUE,
                        ESP_ERR_NOT_FOUND, TAG,
                        "WHO_AM_I不匹配: 0x%02X", who);

    ESP_RETURN_ON_ERROR(device_write(handle->device, ICM_REG_PWR_MGMT_1, 0x80),
                        TAG, "复位失败");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(select_bank(handle, 0), TAG, "选择Bank 0失败");
    ESP_RETURN_ON_ERROR(device_write(handle->device, ICM_REG_PWR_MGMT_1, 0x01),
                        TAG, "唤醒失败");
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t user_ctrl = 0;
    ESP_RETURN_ON_ERROR(device_read(handle->device, ICM_REG_USER_CTRL,
                                    &user_ctrl, 1),
                        TAG, "读取USER_CTRL失败");
    user_ctrl &= (uint8_t)~0x20;
    ESP_RETURN_ON_ERROR(device_write(handle->device, ICM_REG_USER_CTRL,
                                     user_ctrl),
                        TAG, "关闭内部I2C主机失败");

    /* 保留原工程的配置值。 */
    ESP_RETURN_ON_ERROR(select_bank(handle, 2), TAG, "选择Bank 2失败");
    ESP_RETURN_ON_ERROR(device_write(handle->device,
                                     ICM_BANK2_GYRO_CONFIG_1, 0x03),
                        TAG, "配置陀螺仪失败");
    ESP_RETURN_ON_ERROR(device_write(handle->device,
                                     ICM_BANK2_ACCEL_CONFIG, 0x03),
                        TAG, "配置加速度计失败");

    ESP_RETURN_ON_ERROR(select_bank(handle, 0), TAG, "选择Bank 0失败");
    ESP_RETURN_ON_ERROR(device_write(handle->device, ICM_REG_INT_PIN_CFG, 0x22),
                        TAG, "开启AK09916旁路失败");
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t mag_who = 0;
    esp_err_t mag_ret = device_read(handle->magnetometer, AK09916_REG_WIA2,
                                    &mag_who, 1);
    if (mag_ret != ESP_OK || mag_who != AK09916_WHO_AM_I_VALUE) {
        handle->mag_present = false;
        ESP_LOGW(TAG, "AK09916未检测到，ICM-20948按六轴模式工作");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(device_write(handle->magnetometer,
                                     AK09916_REG_CNTL3, 0x01),
                        TAG, "复位AK09916失败");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(device_write(handle->magnetometer,
                                     AK09916_REG_CNTL2, 0x08),
                        TAG, "设置AK09916连续100Hz模式失败");
    vTaskDelay(pdMS_TO_TICKS(10));

    handle->mag_present = true;
    ESP_LOGI(TAG, "ICM-20948和AK09916初始化成功");
    return ESP_OK;
}

static esp_err_t read_magnetometer(icm20948_handle_t *handle,
                                   int16_t *mx, int16_t *my, int16_t *mz)
{
    uint8_t st1 = 0;
    ESP_RETURN_ON_ERROR(device_read(handle->magnetometer, AK09916_REG_ST1,
                                    &st1, 1),
                        TAG, "读取磁力计状态失败");
    if ((st1 & 0x01) == 0) {
        return ESP_ERR_NOT_FINISHED;
    }

    uint8_t raw[8];
    ESP_RETURN_ON_ERROR(device_read(handle->magnetometer, AK09916_REG_HXL,
                                    raw, sizeof(raw)),
                        TAG, "读取磁力计数据失败");
    if ((raw[7] & 0x08) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *mx = read_le16(&raw[0]);
    *my = read_le16(&raw[2]);
    *mz = read_le16(&raw[4]);
    return ESP_OK;
}

esp_err_t icm20948_read_raw(icm20948_handle_t *handle,
                            icm20948_raw_data_t *data)
{
    ESP_RETURN_ON_FALSE(handle != NULL && handle->device != NULL && data != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "参数无效");
    memset(data, 0, sizeof(*data));

    ESP_RETURN_ON_ERROR(select_bank(handle, 0), TAG, "选择Bank 0失败");
    uint8_t raw[12];
    ESP_RETURN_ON_ERROR(device_read(handle->device, ICM_REG_ACCEL_XOUT_H,
                                    raw, sizeof(raw)),
                        TAG, "读取六轴数据失败");

    data->ax = read_be16(&raw[0]);
    data->ay = read_be16(&raw[2]);
    data->az = read_be16(&raw[4]);
    data->gx = read_be16(&raw[6]);
    data->gy = read_be16(&raw[8]);
    data->gz = read_be16(&raw[10]);

    if (handle->mag_present) {
        int16_t mx = 0;
        int16_t my = 0;
        int16_t mz = 0;
        if (read_magnetometer(handle, &mx, &my, &mz) == ESP_OK) {
            handle->last_mx = mx;
            handle->last_my = my;
            handle->last_mz = mz;
            handle->last_mag_valid = true;
        }
    }

    if (handle->last_mag_valid) {
        data->mx = handle->last_mx;
        data->my = handle->last_my;
        data->mz = handle->last_mz;
        data->mag_valid = true;
    }
    return ESP_OK;
}
