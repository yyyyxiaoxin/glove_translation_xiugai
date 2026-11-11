#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int raw_adc;
    uint32_t voltage_mv;
    uint8_t percentage;
    int64_t timestamp_ms;
    uint32_t sample_sequence;
    bool valid;
} battery_status_t;

/* 初始化 GPIO1/ADC1_CH0，并启动 1 Hz 电池采样任务。 */
esp_err_t battery_monitor_init(void);

/* 获取最近一次经过 8 点滑动平均后的电池状态。 */
bool battery_monitor_get_status(battery_status_t *status);

#ifdef __cplusplus
}
#endif
