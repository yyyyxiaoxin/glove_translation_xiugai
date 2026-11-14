#pragma once
#include "esp_err.h"
#include "imu_acquisition.h"

esp_err_t cloud_upload_init(void);
bool cloud_upload_has_pending(void);

/* 本机采集帧。 */
void cloud_upload_submit(const imu_frame_t *frame);

/*
 * 网关转发帧：左手把右手的 ESP-NOW 帧按右手自己的 MAC/session 上传。
 * src_mac 为发送方 STA MAC，session 为右手本机上电 session。
 * 可在 Wi-Fi 接收回调中调用（只做入队，不阻塞）。
 */
void cloud_upload_submit_remote(const uint8_t src_mac[6], uint64_t session,
                                const imu_frame_t *frame);
