#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "imu_acquisition.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start Wi-Fi STA mode and reconnect automatically if the hotspot drops. */
esp_err_t wifi_transport_init(void);
esp_err_t wifi_transport_scan(uint16_t *count, wifi_ap_record_t *records);

/* Send one raw IMU CSV frame through UDP broadcast when Wi-Fi is ready. */
esp_err_t wifi_transport_send_frame(const imu_frame_t *frame);

/* 独立发送电池状态，不改变固定 72 字段的 IMU 数据帧。 */
esp_err_t wifi_transport_send_battery(int64_t timestamp_ms,
                                      uint32_t voltage_mv,
                                      uint8_t percentage);

bool wifi_transport_is_connected(void);

#ifdef __cplusplus
}
#endif
