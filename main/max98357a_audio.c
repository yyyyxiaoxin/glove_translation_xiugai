#include "max98357a_audio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"
#if CONFIG_GLOVE_HAND_LEFT
#include "recording_voice_pcm.h"
#endif

#define AUDIO_SAMPLE_RATE_HZ       16000
#define TEST_TONE_FREQUENCY_HZ     1000
#define TEST_TONE_DURATION_MS      500
#ifndef TEST_TONE_AMPLITUDE
#define TEST_TONE_AMPLITUDE        2000
#endif
#define TEST_TONE_FRAMES_PER_WRITE 64

static const char *TAG = "max98357a";
static i2s_chan_handle_t s_tx_channel;
static bool s_initialized;
static QueueHandle_t s_voice_queue;
#if CONFIG_GLOVE_HAND_LEFT
static void voice_task(void *arg);
#endif

esp_err_t max98357a_audio_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    const gpio_config_t sd_config = {
        .pin_bit_mask = 1ULL << MAX98357A_SD_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&sd_config), TAG, "配置SD/MODE引脚失败");
    ESP_RETURN_ON_ERROR(gpio_set_level(MAX98357A_SD_GPIO, 0),
                        TAG, "关闭功放失败");

    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &s_tx_channel, NULL),
                        TAG, "创建I2S发送通道失败");

    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MAX98357A_BCLK_GPIO,
            .ws = MAX98357A_LRCLK_GPIO,
            .dout = MAX98357A_DIN_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_tx_channel, &standard_config),
        TAG, "初始化I2S标准模式失败");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_channel),
                        TAG, "启用I2S通道失败");

    s_initialized = true;
#if CONFIG_GLOVE_HAND_LEFT
    s_voice_queue = xQueueCreate(8, sizeof(bool));
    if (!s_voice_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(voice_task, "recording_voice", 3072, NULL, 2, NULL) != pdPASS) {
        vQueueDelete(s_voice_queue);
        s_voice_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
#endif
    ESP_LOGI(TAG,
             "I2S发送端初始化完成（未验证功放硬件）: BCLK=GPIO%d LRCLK=GPIO%d DIN=GPIO%d SD=GPIO%d",
             MAX98357A_BCLK_GPIO, MAX98357A_LRCLK_GPIO,
             MAX98357A_DIN_GPIO, MAX98357A_SD_GPIO);
    return ESP_OK;
}

#if CONFIG_GLOVE_HAND_LEFT
static void voice_task(void *arg)
{
    (void)arg;
#if CONFIG_GLOVE_HAND_LEFT
    bool active;
    int16_t stereo[256 * 2];
    for (;;) {
        if (xQueueReceive(s_voice_queue, &active, portMAX_DELAY) != pdTRUE) continue;
        const int8_t *pcm = active ? s_voice_start : s_voice_stop;
        size_t count = active ? sizeof(s_voice_start) : sizeof(s_voice_stop);
        gpio_set_level(MAX98357A_SD_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        esp_err_t ret = ESP_OK;
        for (size_t offset = 0; offset < count && ret == ESP_OK; offset += 128) {
            size_t chunk = count - offset;
            if (chunk > 128) chunk = 128;
            for (size_t i = 0; i < chunk; ++i) {
                // Repeat each 8 kHz mono sample twice for 16 kHz stereo I2S.
                for (size_t j = 0; j < 4; ++j) stereo[i * 4 + j] = (int16_t)pcm[offset + i] * 128;
            }
            size_t bytes = chunk * 4 * sizeof(int16_t), sent = 0;
            while (sent < bytes && ret == ESP_OK) {
                size_t written = 0;
                ret = i2s_channel_write(s_tx_channel, (uint8_t *)stereo + sent,
                                        bytes - sent, &written, 1000);
                sent += written;
                if (written == 0 && ret == ESP_OK) ret = ESP_FAIL;
            }
        }
        memset(stereo, 0, sizeof(stereo));
        for (int i = 0; i < 4; ++i) {
            size_t written;
            i2s_channel_write(s_tx_channel, stereo, sizeof(stereo), &written, 1000);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
        gpio_set_level(MAX98357A_SD_GPIO, 0);
        ESP_LOGI(TAG, "Recording speech %s: %s", active ? "start" : "stop", esp_err_to_name(ret));
    }
#else
    vTaskDelete(NULL);
#endif
}
#endif

esp_err_t max98357a_announce_recording(bool active)
{
    if (!s_voice_queue) return ESP_ERR_INVALID_STATE;
    return xQueueSend(s_voice_queue, &active, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t max98357a_play_test_tone(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE,
                        TAG, "音频模块尚未初始化");

    /* 16 kHz 下每周期16点，左右声道发送相同样本。 */
    static const int16_t sine_cycle[16] = {
        0, 765, 1414, 1848, 2000, 1848, 1414, 765,
        0, -765, -1414, -1848, -2000, -1848, -1414, -765,
    };
    int16_t stereo[TEST_TONE_FRAMES_PER_WRITE * 2];
    for (size_t frame = 0; frame < TEST_TONE_FRAMES_PER_WRITE; ++frame) {
        int16_t sample = (int32_t)sine_cycle[frame % 16] *
                         TEST_TONE_AMPLITUDE / 2000;
        stereo[frame * 2] = sample;
        stereo[frame * 2 + 1] = sample;
    }

    ESP_RETURN_ON_ERROR(gpio_set_level(MAX98357A_SD_GPIO, 1),
                        TAG, "启用功放失败");
    vTaskDelay(pdMS_TO_TICKS(10));

    const size_t total_frames =
        AUDIO_SAMPLE_RATE_HZ * TEST_TONE_DURATION_MS / 1000;
    size_t frames_sent = 0;
    while (frames_sent < total_frames) {
        size_t frames = total_frames - frames_sent;
        if (frames > TEST_TONE_FRAMES_PER_WRITE) {
            frames = TEST_TONE_FRAMES_PER_WRITE;
        }

        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(s_tx_channel, stereo,
                                          frames * 2 * sizeof(int16_t),
                                          &bytes_written, portMAX_DELAY);
        if (ret != ESP_OK) {
            gpio_set_level(MAX98357A_SD_GPIO, 0);
            return ret;
        }
        frames_sent += bytes_written / (2 * sizeof(int16_t));
    }

    memset(stereo, 0, sizeof(stereo));
    for (int i = 0; i < 4; ++i) {
        size_t bytes_written = 0;
        (void)i2s_channel_write(s_tx_channel, stereo, sizeof(stereo),
                                &bytes_written, portMAX_DELAY);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(gpio_set_level(MAX98357A_SD_GPIO, 0),
                        TAG, "测试音后关闭功放失败");

    ESP_LOGI(TAG, "%d Hz测试音数据发送完成，数字峰值=%d/32767（实际发声需听音确认）",
             TEST_TONE_FREQUENCY_HZ, TEST_TONE_AMPLITUDE);
    return ESP_OK;
}
