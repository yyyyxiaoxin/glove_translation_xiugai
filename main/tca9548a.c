#include "tca9548a.h"

#include <stddef.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TCA9548A_WRITE_TIMEOUT_MS  100

static const char *TAG = "tca9548a";

static esp_err_t tca9548a_write_control(tca9548a_handle_t *handle,
                                       uint8_t control)
{
    ESP_RETURN_ON_FALSE(handle != NULL && handle->device != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "驱动尚未初始化");

    return i2c_master_transmit(handle->device, &control, sizeof(control),
                               TCA9548A_WRITE_TIMEOUT_MS);
}

esp_err_t tca9548a_init(tca9548a_handle_t *handle,
                       i2c_master_bus_handle_t bus_handle,
                       uint8_t address,
                       uint32_t scl_speed_hz)
{
    ESP_RETURN_ON_FALSE(handle != NULL && bus_handle != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "handle 或 I2C bus 为空");
    ESP_RETURN_ON_FALSE(address <= 0x7F && scl_speed_hz > 0,
                        ESP_ERR_INVALID_ARG, TAG, "I2C 地址或频率无效");

    *handle = (tca9548a_handle_t) {
        .address = address,
        .current_channel = TCA9548A_NO_CHANNEL,
        .present = false,
    };

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = scl_speed_hz,
    };

    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus_handle, &device_config,
                                                   &handle->device),
                        TAG, "添加 TCA9548A I2C 设备失败");

    esp_err_t ret = tca9548a_write_control(handle, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "地址 0x%02X 无响应", address);
        i2c_master_bus_rm_device(handle->device);
        handle->device = NULL;
        return ret;
    }

    handle->present = true;
    ESP_LOGI(TAG, "检测到 TCA9548A，地址 0x%02X", address);
    return ESP_OK;
}

esp_err_t tca9548a_select_channel(tca9548a_handle_t *handle, uint8_t channel)
{
    ESP_RETURN_ON_FALSE(handle != NULL && handle->present,
                        ESP_ERR_INVALID_STATE, TAG, "TCA9548A 不在线");
    ESP_RETURN_ON_FALSE(channel < TCA9548A_CHANNEL_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "通道必须为 0~7");

    if (handle->current_channel == channel) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(tca9548a_write_control(handle, (uint8_t)(1U << channel)),
                        TAG, "选择通道 %u 失败", channel);
    handle->current_channel = channel;
    /* The selected downstream channel is active after the control write STOP.
     * No scheduler delay is needed before the next I2C transaction. */
    return ESP_OK;
}

esp_err_t tca9548a_disable_all_channels(tca9548a_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL && handle->present,
                        ESP_ERR_INVALID_STATE, TAG, "TCA9548A 不在线");

    ESP_RETURN_ON_ERROR(tca9548a_write_control(handle, 0x00),
                        TAG, "关闭全部通道失败");
    handle->current_channel = TCA9548A_NO_CHANNEL;
    return ESP_OK;
}

uint8_t tca9548a_get_current_channel(const tca9548a_handle_t *handle)
{
    return handle != NULL ? handle->current_channel : TCA9548A_NO_CHANNEL;
}

bool tca9548a_is_present(const tca9548a_handle_t *handle)
{
    return handle != NULL && handle->present;
}
