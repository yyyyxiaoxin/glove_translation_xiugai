#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCA9548A_DEFAULT_ADDRESS  0x70
#define TCA9548A_CHANNEL_COUNT    8
#define TCA9548A_NO_CHANNEL       0xFF

typedef struct {
    i2c_master_dev_handle_t device;
    uint8_t address;
    uint8_t current_channel;
    bool present;
} tca9548a_handle_t;

/** 初始化并探测一颗 TCA9548A，成功后关闭全部下游通道。 */
esp_err_t tca9548a_init(tca9548a_handle_t *handle,
                       i2c_master_bus_handle_t bus_handle,
                       uint8_t address,
                       uint32_t scl_speed_hz);

/** 单独启用一个下游通道（0~7），其他通道同时关闭。 */
esp_err_t tca9548a_select_channel(tca9548a_handle_t *handle, uint8_t channel);

/** 关闭全部下游通道。 */
esp_err_t tca9548a_disable_all_channels(tca9548a_handle_t *handle);

uint8_t tca9548a_get_current_channel(const tca9548a_handle_t *handle);
bool tca9548a_is_present(const tca9548a_handle_t *handle);

#ifdef __cplusplus
}
#endif
