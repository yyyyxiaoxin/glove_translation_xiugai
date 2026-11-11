#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "imu_acquisition.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ESP-NOW 左右手链路。
 *
 * 右手每采集一帧就通过 ESP-NOW 广播一帧；左手只接收对侧手的帧，
 * 交给注册的回调（云端上传模块）作为第二个数据源。
 *
 * 接收回调运行在 Wi-Fi 任务上下文中，必须快速返回，不能阻塞。
 * src_mac 为发送方 STA MAC（右手的 device id）。
 */
typedef void (*glove_link_rx_cb_t)(const uint8_t src_mac[6], uint64_t session,
                                   const imu_frame_t *frame);

/* 必须在 Wi-Fi 启动（wifi_transport_init）之后调用。 */
esp_err_t glove_link_init(void);

/* 仅右手发送；左手调用会返回 ESP_OK 但不发送。 */
esp_err_t glove_link_send_frame(const imu_frame_t *frame);

/*
 * 左手广播录制开始/停止（active=true 时 recording_id 必须非 0）。
 * 右手收到后开始/停止发送 IMU 帧，并使用同一个 session。
 * 录制期间左手每秒调用一次作为心跳；右手 3 秒收不到即自动停止。
 */
esp_err_t glove_link_send_recording(bool active, uint64_t recording_id);

void glove_link_register_rx_cb(glove_link_rx_cb_t cb);

bool glove_link_is_ready(void);

#ifdef __cplusplus
}
#endif
