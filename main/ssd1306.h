#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define SSD1306_DEFAULT_ADDRESS 0x3C
#define SSD1306_WIDTH           128
#define SSD1306_HEIGHT          64

typedef struct {
    i2c_master_dev_handle_t device;
    uint8_t framebuffer[SSD1306_WIDTH * SSD1306_HEIGHT / 8];
} ssd1306_handle_t;

esp_err_t ssd1306_init(ssd1306_handle_t *handle,
                       i2c_master_bus_handle_t bus_handle,
                       uint8_t address,
                       uint32_t scl_speed_hz);
void ssd1306_clear(ssd1306_handle_t *handle);
void ssd1306_draw_text(ssd1306_handle_t *handle, uint8_t column,
                       uint8_t page, const char *text);
esp_err_t ssd1306_update(ssd1306_handle_t *handle);
