#include "glove_recording.h"

#include <inttypes.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define RECORDING_VALID_UNIX 1704067200

static const char *TAG = "glove_recording";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_active;
static uint64_t s_id;
static uint32_t s_generation;

/*
 * session 高 32 位为 UTC 秒（SNTP 有效时），低 32 位随机。
 * 页面据此显示可读时间；未校时时退化为 64 位随机数。
 */
static uint64_t make_recording_id(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    uint32_t seconds = 0;
    if (now.tv_sec >= RECORDING_VALID_UNIX && now.tv_sec <= 4102444800LL) {
        seconds = (uint32_t)now.tv_sec;
    }

    uint64_t id;
    if (seconds != 0) {
        id = ((uint64_t)seconds << 32) | esp_random();
    } else {
        id = ((uint64_t)esp_random() << 32) | esp_random();
    }
    return id != 0 ? id : 1;
}

bool glove_recording_start(void)
{
    uint64_t id = make_recording_id();
    bool started = false;

    portENTER_CRITICAL(&s_lock);
    if (!s_active) {
        s_active = true;
        s_id = id;
        ++s_generation;
        started = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (!started) {
        ESP_LOGW(TAG, "已在录制中（session=%016" PRIx64 "），本次开始键忽略",
                 glove_recording_id());
        return false;
    }

    ESP_LOGI(TAG, "开始录制：session=%016" PRIx64 "（左右手共用），再次按GPIO4结束",
             id);
    return true;
}

bool glove_recording_stop(void)
{
    uint64_t stopped_id = 0;
    bool stopped = false;

    portENTER_CRITICAL(&s_lock);
    if (s_active) {
        stopped_id = s_id;
        s_active = false;
        s_id = 0;
        ++s_generation;
        stopped = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (!stopped) {
        ESP_LOGW(TAG, "当前没有进行中的录制，本次停止键忽略");
        return false;
    }

    ESP_LOGI(TAG, "停止录制：session=%016" PRIx64 "，等待剩余帧上传",
             stopped_id);
    return true;
}

void glove_recording_set_remote(uint64_t recording_id)
{
    bool changed = false;
    bool active = recording_id != 0;

    portENTER_CRITICAL(&s_lock);
    if (active) {
        if (!s_active || s_id != recording_id) {
            s_active = true;
            s_id = recording_id;
            ++s_generation;
            changed = true;
        }
    } else if (s_active) {
        s_active = false;
        s_id = 0;
        ++s_generation;
        changed = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (!changed) {
        return; /* 心跳重复包 */
    }
    if (active) {
        ESP_LOGI(TAG, "收到左手录制指令：开始 session=%016" PRIx64, recording_id);
    } else {
        ESP_LOGI(TAG, "收到左手录制指令：停止");
    }
}

bool glove_recording_get(uint64_t *recording_id)
{
    bool active;
    portENTER_CRITICAL(&s_lock);
    active = s_active;
    if (recording_id != NULL) {
        *recording_id = s_active ? s_id : 0;
    }
    portEXIT_CRITICAL(&s_lock);
    return active;
}

bool glove_recording_active(void)
{
    return glove_recording_get(NULL);
}

uint64_t glove_recording_id(void)
{
    uint64_t id = 0;
    glove_recording_get(&id);
    return id;
}

uint32_t glove_recording_generation(void)
{
    uint32_t generation;
    portENTER_CRITICAL(&s_lock);
    generation = s_generation;
    portEXIT_CRITICAL(&s_lock);
    return generation;
}
