#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "imu_acquisition.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GLOVE_INFERENCE_SENTENCE = 0,
    GLOVE_INFERENCE_WORD,
} glove_inference_mode_t;

typedef enum {
    GLOVE_OPERATION_AUTO = 0,
    GLOVE_OPERATION_MANUAL,
} glove_operation_mode_t;

typedef enum {
    GLOVE_OPERATION_AUTO_STANDBY = 0,
    GLOVE_OPERATION_MANUAL_SLEEP,
} glove_operation_state_t;

typedef struct {
    int32_t ax_min;
    int32_t ax_max;
    int32_t ay_min;
    int32_t ay_max;
    int32_t az_min;
    int32_t az_max;
    bool valid;
} glove_accel_window_t;

/* 初始化按键GPIO并启动20 ms轮询、60 ms消抖任务。 */
esp_err_t glove_buttons_init(void);

/*
 * 由采集循环传入最新原始帧，完成PC7对应的延时、采样和阈值计算。
 * 此接口不会额外访问I2C，因此不会与IMU采集争用总线。
 */
void glove_buttons_process_imu_frame(const imu_frame_t *frame);

glove_inference_mode_t glove_buttons_get_inference_mode(void);
glove_operation_mode_t glove_buttons_get_operation_mode(void);
glove_operation_state_t glove_buttons_get_operation_state(void);
bool glove_buttons_calibration_busy(void);

/* 校准完成后读取某一路加速度窗口；尚无有效结果时返回ESP_ERR_NOT_FOUND。 */
esp_err_t glove_buttons_get_accel_window(int channel,
                                         glove_accel_window_t *window);

#ifdef __cplusplus
}
#endif
