#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "imu_acquisition.h"

#define GLOVE_SYNC_MAGIC 0x33535647u
esp_err_t glove_sync_init(void);
void glove_sync_receive(const uint8_t *data, int len, int64_t received_us);
/* Wait for the next 20 ms slot, then freeze the clock estimate for this scan. */
void glove_sync_wait_frame(imu_frame_t *frame);
