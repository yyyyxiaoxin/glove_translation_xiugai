#include "glove_buttons.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "glove_identity.h"
#include "glove_recording.h"
#include "max98357a_audio.h"

#define BUTTON_TASK_STACK_SIZE  3072
#define BUTTON_TASK_PRIORITY    5
#define BUTTON_DEBOUNCE_COUNT   (BUTTON_DEBOUNCE_MS / BUTTON_POLL_INTERVAL_MS)

typedef void (*button_callback_t)(void);

typedef struct {
    gpio_num_t pin;
    bool pressed;
    unsigned int debounce_count;
    button_callback_t on_press;
} button_state_t;

typedef enum {
    CALIBRATION_IDLE = 0,
    CALIBRATION_WAITING,
    CALIBRATION_COLLECTING,
} calibration_state_t;

typedef struct {
    int32_t min_ax;
    int32_t max_ax;
    int32_t min_ay;
    int32_t max_ay;
    int32_t min_az;
    int32_t max_az;
    bool valid;
} calibration_tracker_t;

static const char *TAG = "glove_buttons";
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static glove_inference_mode_t s_inference_mode = GLOVE_INFERENCE_SENTENCE;
static glove_operation_mode_t s_operation_mode = GLOVE_OPERATION_AUTO;
static glove_operation_state_t s_operation_state =
    GLOVE_OPERATION_AUTO_STANDBY;
static calibration_state_t s_calibration_state = CALIBRATION_IDLE;
static bool s_calibration_requested;
static bool s_initialized;

static int64_t s_calibration_start_ms;
static int64_t s_next_sample_ms;
static unsigned int s_sample_count;
static calibration_tracker_t s_tracker[IMU_TOTAL_CHANNELS];
static glove_accel_window_t s_accel_windows[IMU_TOTAL_CHANNELS];

#if CONFIG_GLOVE_HAND_LEFT
static void handle_recording_stop_press(void)
{
    ESP_LOGI(TAG, "GPIO%d停止采集键", BUTTON_OPERATION_MODE_GPIO);
    if (glove_recording_stop()) max98357a_announce_recording(false);
}
#else
static void handle_operation_press(void)
{
    glove_operation_mode_t mode;
    glove_operation_state_t state;

    portENTER_CRITICAL(&s_state_lock);
    if (s_operation_mode == GLOVE_OPERATION_AUTO) {
        s_operation_mode = GLOVE_OPERATION_MANUAL;
        s_operation_state = GLOVE_OPERATION_MANUAL_SLEEP;
    } else {
        s_operation_mode = GLOVE_OPERATION_AUTO;
        s_operation_state = GLOVE_OPERATION_AUTO_STANDBY;
    }
    mode = s_operation_mode;
    state = s_operation_state;
    portEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(TAG, "GPIO%d工作模式键：mode=%s, state=%s",
             BUTTON_OPERATION_MODE_GPIO,
             mode == GLOVE_OPERATION_AUTO ? "AUTO" : "MANUAL",
             state == GLOVE_OPERATION_AUTO_STANDBY ? "AUTO_STANDBY"
                                                    : "MANUAL_SLEEP");
}
#endif

static void handle_calibration_press(void)
{
    bool accepted = false;

    portENTER_CRITICAL(&s_state_lock);
    if (s_calibration_state == CALIBRATION_IDLE &&
        !s_calibration_requested) {
        s_calibration_requested = true;
        accepted = true;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (accepted) {
        ESP_LOGI(TAG, "GPIO%d姿态校准键：请求已接收",
                 BUTTON_CALIBRATION_GPIO);
    } else {
        ESP_LOGW(TAG, "姿态校准正在进行，本次按键忽略");
    }
}

static void poll_one_button(button_state_t *button)
{
    int level = gpio_get_level(button->pin);

    if (!button->pressed) {
        if (level == 0) {
            if (++button->debounce_count >= BUTTON_DEBOUNCE_COUNT) {
                button->pressed = true;
                button->debounce_count = 0;
                button->on_press();
            }
        } else {
            button->debounce_count = 0;
        }
    } else {
        if (level != 0) {
            if (++button->debounce_count >= BUTTON_DEBOUNCE_COUNT) {
                button->pressed = false;
                button->debounce_count = 0;
            }
        } else {
            button->debounce_count = 0;
        }
    }
}

static void button_task(void *argument)
{
    (void)argument;

    button_state_t buttons[] = {
#if CONFIG_GLOVE_HAND_LEFT
        {
            .pin = BUTTON_OPERATION_MODE_GPIO,
            .on_press = handle_recording_stop_press,
        },
#else
        {
            .pin = BUTTON_OPERATION_MODE_GPIO,
            .on_press = handle_operation_press,
        },
#endif
        {
            .pin = BUTTON_CALIBRATION_GPIO,
            .on_press = handle_calibration_press,
        },
    };

    while (true) {
        for (unsigned int i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
            poll_one_button(&buttons[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_INTERVAL_MS));
    }
}

esp_err_t glove_buttons_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    uint64_t pin_mask = (1ULL << BUTTON_OPERATION_MODE_GPIO) |
                        (1ULL << BUTTON_CALIBRATION_GPIO);

    const gpio_config_t input_config = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&input_config);
    if (ret != ESP_OK) {
        return ret;
    }

    BaseType_t task_result = xTaskCreate(button_task, "glove_buttons",
                                         BUTTON_TASK_STACK_SIZE, NULL,
                                         BUTTON_TASK_PRIORITY, NULL);
    if (task_result != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
#if CONFIG_GLOVE_HAND_LEFT
    ESP_LOGI(TAG, "左手按键初始化：GPIO%d=禁用（串口F2控制录制），GPIO%d=备用停止，GPIO%d=姿态校准",
             BUTTON_INFERENCE_GPIO, BUTTON_OPERATION_MODE_GPIO,
             BUTTON_CALIBRATION_GPIO);
#else
    ESP_LOGI(TAG, "右手按键初始化：GPIO%d=工作模式，GPIO%d=姿态校准；GPIO%d为左手预留",
             BUTTON_OPERATION_MODE_GPIO, BUTTON_CALIBRATION_GPIO,
             BUTTON_INFERENCE_GPIO);
#endif
    ESP_LOGI(TAG, "按键均为内部上拉、按下接GND、低电平有效，消抖%d ms",
             BUTTON_DEBOUNCE_MS);
    return ESP_OK;
}

static void reset_calibration_trackers(void)
{
    for (int channel = 0; channel < IMU_TOTAL_CHANNELS; ++channel) {
        s_tracker[channel] = (calibration_tracker_t) {
            .min_ax = INT32_MAX,
            .max_ax = INT32_MIN,
            .min_ay = INT32_MAX,
            .max_ay = INT32_MIN,
            .min_az = INT32_MAX,
            .max_az = INT32_MIN,
            .valid = false,
        };
    }
    s_sample_count = 0;
}

static void collect_calibration_sample(const imu_frame_t *frame)
{
    for (int channel = 0; channel < IMU_TOTAL_CHANNELS; ++channel) {
        const imu_channel_data_t *data = &frame->channel[channel];
        if (!data->valid) {
            continue;
        }

        calibration_tracker_t *tracker = &s_tracker[channel];
        if (data->ax < tracker->min_ax) tracker->min_ax = data->ax;
        if (data->ax > tracker->max_ax) tracker->max_ax = data->ax;
        if (data->ay < tracker->min_ay) tracker->min_ay = data->ay;
        if (data->ay > tracker->max_ay) tracker->max_ay = data->ay;
        if (data->az < tracker->min_az) tracker->min_az = data->az;
        if (data->az > tracker->max_az) tracker->max_az = data->az;
        tracker->valid = true;
    }
    ++s_sample_count;
}

static void finish_calibration(void)
{
    glove_accel_window_t result[IMU_TOTAL_CHANNELS] = {0};
    unsigned int valid_count = 0;

    for (int channel = 0; channel < IMU_TOTAL_CHANNELS; ++channel) {
        const calibration_tracker_t *tracker = &s_tracker[channel];
        if (!tracker->valid) {
            continue;
        }

        result[channel] = (glove_accel_window_t) {
            .ax_min = tracker->min_ax - CALIBRATION_MARGIN,
            .ax_max = tracker->max_ax + CALIBRATION_MARGIN,
            .ay_min = tracker->min_ay - CALIBRATION_MARGIN,
            .ay_max = tracker->max_ay + CALIBRATION_MARGIN,
            .az_min = tracker->min_az - CALIBRATION_MARGIN,
            .az_max = tracker->max_az + CALIBRATION_MARGIN,
            .valid = true,
        };
        ++valid_count;
    }

    portENTER_CRITICAL(&s_state_lock);
    memcpy(s_accel_windows, result, sizeof(s_accel_windows));
    s_calibration_state = CALIBRATION_IDLE;
    portEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(TAG, "姿态校准完成：%u组样本，%u/%d路有效，hand=%s",
             s_sample_count, valid_count, IMU_TOTAL_CHANNELS, GLOVE_HAND_NAME);
    for (int channel = 0; channel < IMU_TOTAL_CHANNELS; ++channel) {
        const glove_accel_window_t *window = &result[channel];
        if (!window->valid) {
            ESP_LOGW(TAG, "CAL CH%d：无有效传感器数据", channel);
            continue;
        }
        ESP_LOGI(TAG,
                 "CAL CH%d：ax=[%" PRId32 ",%" PRId32 "] ay=[%" PRId32 ",%" PRId32 "] az=[%" PRId32 ",%" PRId32 "]",
                 channel, window->ax_min, window->ax_max,
                 window->ay_min, window->ay_max,
                 window->az_min, window->az_max);
    }
    ESP_LOGI(TAG, "校准阈值当前保存在RAM中，断电或复位后恢复默认状态");
}

void glove_buttons_process_imu_frame(const imu_frame_t *frame)
{
    if (!s_initialized || frame == NULL) {
        return;
    }

    bool begin_request = false;
    portENTER_CRITICAL(&s_state_lock);
    if (s_calibration_requested &&
        s_calibration_state == CALIBRATION_IDLE) {
        s_calibration_requested = false;
        s_calibration_state = CALIBRATION_WAITING;
        begin_request = true;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (begin_request) {
        s_calibration_start_ms = frame->timestamp_ms;
        ESP_LOGI(TAG, "姿态校准将在%d ms后开始，请摆好自动唤醒姿势",
                 CALIBRATION_WAIT_MS);
    }

    calibration_state_t state;
    portENTER_CRITICAL(&s_state_lock);
    state = s_calibration_state;
    portEXIT_CRITICAL(&s_state_lock);

    if (state == CALIBRATION_WAITING &&
        frame->timestamp_ms - s_calibration_start_ms >= CALIBRATION_WAIT_MS) {
        reset_calibration_trackers();
        s_next_sample_ms = frame->timestamp_ms;
        portENTER_CRITICAL(&s_state_lock);
        s_calibration_state = CALIBRATION_COLLECTING;
        portEXIT_CRITICAL(&s_state_lock);
        state = CALIBRATION_COLLECTING;
        ESP_LOGI(TAG, "开始采集姿态阈值：%d组，目标间隔%d ms",
                 CALIBRATION_SAMPLE_COUNT,
                 CALIBRATION_SAMPLE_INTERVAL_MS);
    }

    if (state != CALIBRATION_COLLECTING ||
        frame->timestamp_ms < s_next_sample_ms) {
        return;
    }

    collect_calibration_sample(frame);
    s_next_sample_ms += CALIBRATION_SAMPLE_INTERVAL_MS;
    if (s_sample_count >= CALIBRATION_SAMPLE_COUNT) {
        finish_calibration();
    }
}

glove_inference_mode_t glove_buttons_get_inference_mode(void)
{
    glove_inference_mode_t mode;
    portENTER_CRITICAL(&s_state_lock);
    mode = s_inference_mode;
    portEXIT_CRITICAL(&s_state_lock);
    return mode;
}

glove_operation_mode_t glove_buttons_get_operation_mode(void)
{
    glove_operation_mode_t mode;
    portENTER_CRITICAL(&s_state_lock);
    mode = s_operation_mode;
    portEXIT_CRITICAL(&s_state_lock);
    return mode;
}

glove_operation_state_t glove_buttons_get_operation_state(void)
{
    glove_operation_state_t state;
    portENTER_CRITICAL(&s_state_lock);
    state = s_operation_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}

bool glove_buttons_calibration_busy(void)
{
    bool busy;
    portENTER_CRITICAL(&s_state_lock);
    busy = s_calibration_requested ||
           s_calibration_state != CALIBRATION_IDLE;
    portEXIT_CRITICAL(&s_state_lock);
    return busy;
}

esp_err_t glove_buttons_get_accel_window(int channel,
                                         glove_accel_window_t *window)
{
    if (channel < 0 || channel >= IMU_TOTAL_CHANNELS || window == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_state_lock);
    *window = s_accel_windows[channel];
    portEXIT_CRITICAL(&s_state_lock);
    return window->valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}
