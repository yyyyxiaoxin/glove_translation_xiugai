#include "glove_sync.h"
#include <inttypes.h>
#include <string.h>
#include "board_config.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "glove_identity.h"

#define PERIOD_US ((int64_t)IMU_SAMPLE_INTERVAL_MS * 1000)
#define WINDOW 16
#define MAX_RTT_US 10000
#define MAX_AGE_US 3000000
typedef struct {
    uint32_t magic, type, sequence, boot;
    int64_t t1, t2, t3;
} sync_packet_t;
typedef struct { int64_t offset, rtt, received; } observation_t;
static const uint8_t broadcast[6] = {255,255,255,255,255,255};
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
#if CONFIG_GLOVE_HAND_RIGHT
static observation_t samples[WINDOW];
static unsigned count, index_next;
static uint32_t peer_boot, request_sequence;
static int64_t pending_t1, accepted_us, last_response_us, offset_us, rtt_us;
#endif
static uint32_t boot_id;
static esp_timer_handle_t wake_timer;
static TaskHandle_t acquisition_task;
static int64_t last_slot = -1;
static const char *TAG = "glove_sync";

static void wake_scan(void *arg) { xTaskNotifyGive(acquisition_task); }

void glove_sync_receive(const uint8_t *data, int len, int64_t received_us)
{
    if (len != sizeof(sync_packet_t)) return;
    sync_packet_t packet;
    memcpy(&packet, data, sizeof(packet));
    if (packet.magic != GLOVE_SYNC_MAGIC) return;
#if CONFIG_GLOVE_HAND_LEFT
    if (packet.type != 1 || packet.t1 <= 0) return;
    packet.type = 2;
    packet.boot = boot_id;
    packet.t2 = received_us;
    packet.t3 = esp_timer_get_time();
    esp_now_send(broadcast, (uint8_t *)&packet, sizeof(packet));
#else
    if (packet.type != 2) return;
    int64_t now = received_us;
    portENTER_CRITICAL(&lock);
    if (packet.sequence != request_sequence || packet.t1 != pending_t1 || pending_t1 == 0) {
        portEXIT_CRITICAL(&lock);
        return;
    }
    pending_t1 = 0; /* Reject duplicate/late replies. */
    int64_t rtt = (now - packet.t1) - (packet.t3 - packet.t2);
    int64_t offset = ((packet.t2 - packet.t1) + (packet.t3 - now)) / 2;
    if (packet.t3 < packet.t2 || rtt < 0 || rtt > MAX_RTT_US) {
        portEXIT_CRITICAL(&lock);
        return;
    }
    if (peer_boot != packet.boot || (last_response_us && now - last_response_us > MAX_AGE_US)) {
        count = index_next = 0;
        accepted_us = 0;
        peer_boot = packet.boot;
    }
    last_response_us = now;
    samples[index_next] = (observation_t){offset, rtt, now};
    index_next = (index_next + 1) % WINDOW;
    if (count < WINDOW) ++count;
    /* Minimum RTT rejects queued radio packets. Only fresh observations qualify. */
    int best = -1;
    unsigned fresh = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (now - samples[i].received > 2000000) continue;
        ++fresh;
        if (best < 0 || samples[i].rtt < samples[best].rtt) best = i;
    }
    if (best >= 0 && fresh >= 4) {
        offset_us = samples[best].offset;
        rtt_us = samples[best].rtt;
        /* Freshness refers to selected observation, not merely latest response. */
        accepted_us = samples[best].received;
    }
    portEXIT_CRITICAL(&lock);
#endif
}

static void sync_task(void *arg)
{
#if CONFIG_GLOVE_HAND_RIGHT
    for (;;) {
        sync_packet_t packet = {.magic=GLOVE_SYNC_MAGIC, .type=1};
        portENTER_CRITICAL(&lock);
        packet.sequence = ++request_sequence;
        packet.t1 = pending_t1 = esp_timer_get_time();
        portEXIT_CRITICAL(&lock);
        esp_now_send(broadcast, (uint8_t *)&packet, sizeof(packet));
        vTaskDelay(pdMS_TO_TICKS(250));
    }
#else
    vTaskDelete(NULL);
#endif
}

esp_err_t glove_sync_init(void)
{
    boot_id = esp_random();
    acquisition_task = xTaskGetCurrentTaskHandle();
    esp_timer_create_args_t timer = {.callback=wake_scan, .name="imu_slot"};
    esp_err_t err = esp_timer_create(&timer, &wake_timer);
    if (err != ESP_OK) return err;
    if (xTaskCreate(sync_task, "clock_sync", 3072, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void glove_sync_wait_frame(imu_frame_t *frame)
{
    memset(frame, 0, sizeof(*frame));
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
#if CONFIG_GLOVE_HAND_LEFT
    bool valid = true;
    int64_t offset = 0, rtt = 0, age = 0;
#else
    int64_t age = accepted_us ? now - accepted_us : MAX_AGE_US + 1;
    bool valid = accepted_us && age <= MAX_AGE_US;
    int64_t offset = valid ? offset_us : 0;
    int64_t rtt = valid ? rtt_us : 0;
#endif
    portEXIT_CRITICAL(&lock);
    int64_t slot = (now + offset + 1000) / PERIOD_US + 1;
    if (valid && slot <= last_slot) slot = last_slot + 1;
    /* On left reboot a new clock epoch can move backwards; freshness expires
       before scheduling resumes. Keep local timer waits bounded regardless. */
    if ((slot * PERIOD_US - offset) - now > 2 * PERIOD_US) slot = (now + offset) / PERIOD_US + 1;
    int64_t target = slot * PERIOD_US - offset;
    ulTaskNotifyTake(pdTRUE, 0);
    int64_t delay = target - esp_timer_get_time();
    if (delay > 0 && wake_timer && esp_timer_start_once(wake_timer, delay) == ESP_OK) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    if (valid) last_slot = slot;
    frame->scheduled_us = valid ? slot * PERIOD_US : -1;
    frame->clock_offset_us = offset;
    frame->sync_rtt_us = (uint32_t)rtt;
    frame->sync_age_ms = (uint32_t)(age / 1000);
    frame->sync_valid = valid;
    static unsigned scans;
    if (++scans % 250 == 0) ESP_LOGI(TAG, "SYNC hand=%s valid=%d offset_us=%" PRId64 " rtt_us=%" PRId64 " age_ms=%" PRId64 " slot=%" PRId64,
        GLOVE_HAND_NAME, valid, offset, rtt, age/1000, frame->scheduled_us);
}
