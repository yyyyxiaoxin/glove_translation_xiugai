#include "ssd1306.h"

#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#define SSD1306_TIMEOUT_MS 100
#define SSD1306_DATA_CHUNK 64

static const char *TAG = "ssd1306";

static esp_err_t send_commands(ssd1306_handle_t *handle,
                               const uint8_t *commands, size_t length)
{
    uint8_t packet[32];
    ESP_RETURN_ON_FALSE(length + 1 <= sizeof(packet), ESP_ERR_INVALID_SIZE,
                        TAG, "命令过长");
    packet[0] = 0x00;
    memcpy(&packet[1], commands, length);
    return i2c_master_transmit(handle->device, packet, length + 1,
                               SSD1306_TIMEOUT_MS);
}

/* 本测试只保留界面所需字符，减少驱动体积。每个字形为 5x7。 */
static void get_glyph(char c, uint8_t glyph[5])
{
    memset(glyph, 0, 5);
    switch (c) {
    case '0': { uint8_t g[5]={0x3E,0x51,0x49,0x45,0x3E}; memcpy(glyph,g,5); break; }
    case '1': { uint8_t g[5]={0x00,0x42,0x7F,0x40,0x00}; memcpy(glyph,g,5); break; }
    case '2': { uint8_t g[5]={0x42,0x61,0x51,0x49,0x46}; memcpy(glyph,g,5); break; }
    case '3': { uint8_t g[5]={0x21,0x41,0x45,0x4B,0x31}; memcpy(glyph,g,5); break; }
    case '4': { uint8_t g[5]={0x18,0x14,0x12,0x7F,0x10}; memcpy(glyph,g,5); break; }
    case '5': { uint8_t g[5]={0x27,0x45,0x45,0x45,0x39}; memcpy(glyph,g,5); break; }
    case '6': { uint8_t g[5]={0x3C,0x4A,0x49,0x49,0x30}; memcpy(glyph,g,5); break; }
    case '7': { uint8_t g[5]={0x01,0x71,0x09,0x05,0x03}; memcpy(glyph,g,5); break; }
    case '8': { uint8_t g[5]={0x36,0x49,0x49,0x49,0x36}; memcpy(glyph,g,5); break; }
    case '9': { uint8_t g[5]={0x06,0x49,0x49,0x29,0x1E}; memcpy(glyph,g,5); break; }
    case 'A': { uint8_t g[5]={0x7E,0x11,0x11,0x11,0x7E}; memcpy(glyph,g,5); break; }
    case 'C': { uint8_t g[5]={0x3E,0x41,0x41,0x41,0x22}; memcpy(glyph,g,5); break; }
    case 'D': { uint8_t g[5]={0x7F,0x41,0x41,0x22,0x1C}; memcpy(glyph,g,5); break; }
    case 'E': { uint8_t g[5]={0x7F,0x49,0x49,0x49,0x41}; memcpy(glyph,g,5); break; }
    case 'G': { uint8_t g[5]={0x3E,0x41,0x49,0x49,0x7A}; memcpy(glyph,g,5); break; }
    case 'I': { uint8_t g[5]={0x00,0x41,0x7F,0x41,0x00}; memcpy(glyph,g,5); break; }
    case 'L': { uint8_t g[5]={0x7F,0x40,0x40,0x40,0x40}; memcpy(glyph,g,5); break; }
    case 'M': { uint8_t g[5]={0x7F,0x02,0x0C,0x02,0x7F}; memcpy(glyph,g,5); break; }
    case 'P': { uint8_t g[5]={0x7F,0x09,0x09,0x09,0x06}; memcpy(glyph,g,5); break; }
    case 'S': { uint8_t g[5]={0x46,0x49,0x49,0x49,0x31}; memcpy(glyph,g,5); break; }
    case 'T': { uint8_t g[5]={0x01,0x01,0x7F,0x01,0x01}; memcpy(glyph,g,5); break; }
    case 'U': { uint8_t g[5]={0x3F,0x40,0x40,0x40,0x3F}; memcpy(glyph,g,5); break; }
    case 'V': { uint8_t g[5]={0x1F,0x20,0x40,0x20,0x1F}; memcpy(glyph,g,5); break; }
    case 'X': { uint8_t g[5]={0x63,0x14,0x08,0x14,0x63}; memcpy(glyph,g,5); break; }
    case 'Y': { uint8_t g[5]={0x03,0x04,0x78,0x04,0x03}; memcpy(glyph,g,5); break; }
    case 'Z': { uint8_t g[5]={0x61,0x51,0x49,0x45,0x43}; memcpy(glyph,g,5); break; }
    case '+': { uint8_t g[5]={0x08,0x08,0x3E,0x08,0x08}; memcpy(glyph,g,5); break; }
    case '-': { uint8_t g[5]={0x08,0x08,0x08,0x08,0x08}; memcpy(glyph,g,5); break; }
    case '.': { uint8_t g[5]={0x00,0x60,0x60,0x00,0x00}; memcpy(glyph,g,5); break; }
    case ':': { uint8_t g[5]={0x00,0x36,0x36,0x00,0x00}; memcpy(glyph,g,5); break; }
    case '/': { uint8_t g[5]={0x20,0x10,0x08,0x04,0x02}; memcpy(glyph,g,5); break; }
    default: break;
    }
}

esp_err_t ssd1306_init(ssd1306_handle_t *handle,
                       i2c_master_bus_handle_t bus_handle,
                       uint8_t address,
                       uint32_t scl_speed_hz)
{
    ESP_RETURN_ON_FALSE(handle != NULL && bus_handle != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "参数无效");
    memset(handle, 0, sizeof(*handle));

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = scl_speed_hz,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus_handle, &config,
                                                   &handle->device),
                        TAG, "添加 I2C 设备失败");

    const uint8_t init_commands[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6,
        0x2E, 0xAF
    };
    ESP_RETURN_ON_ERROR(send_commands(handle, init_commands,
                                       sizeof(init_commands)),
                        TAG, "初始化命令发送失败");
    ssd1306_clear(handle);
    ESP_RETURN_ON_ERROR(ssd1306_update(handle), TAG, "首次清屏失败");
    ESP_LOGI(TAG, "SSD1306 128x64 初始化成功");
    return ESP_OK;
}

void ssd1306_clear(ssd1306_handle_t *handle)
{
    if (handle != NULL) {
        memset(handle->framebuffer, 0, sizeof(handle->framebuffer));
    }
}

void ssd1306_draw_text(ssd1306_handle_t *handle, uint8_t column,
                       uint8_t page, const char *text)
{
    if (handle == NULL || text == NULL || page >= 8) {
        return;
    }
    size_t offset = (size_t)page * SSD1306_WIDTH + column;
    while (*text != '\0' && offset + 5 < (size_t)(page + 1) * SSD1306_WIDTH) {
        uint8_t glyph[5];
        get_glyph(*text++, glyph);
        memcpy(&handle->framebuffer[offset], glyph, 5);
        handle->framebuffer[offset + 5] = 0;
        offset += 6;
    }
}

esp_err_t ssd1306_update(ssd1306_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL && handle->device != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "驱动未初始化");
    const uint8_t window[] = {0x21, 0x00, 0x7F, 0x22, 0x00, 0x07};
    ESP_RETURN_ON_ERROR(send_commands(handle, window, sizeof(window)),
                        TAG, "设置显示窗口失败");

    uint8_t packet[SSD1306_DATA_CHUNK + 1];
    packet[0] = 0x40;
    for (size_t offset = 0; offset < sizeof(handle->framebuffer);
         offset += SSD1306_DATA_CHUNK) {
        memcpy(&packet[1], &handle->framebuffer[offset], SSD1306_DATA_CHUNK);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(handle->device, packet,
                                                sizeof(packet),
                                                SSD1306_TIMEOUT_MS),
                            TAG, "刷新显示失败");
    }
    return ESP_OK;
}
