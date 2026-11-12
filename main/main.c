#include <inttypes.h>
#include <stdio.h>

#include "battery_monitor.h"
#include "cloud_upload.h"
#include "cloud_presence.h"
#include "board_config.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "glove_buttons.h"
#include "glove_identity.h"
#include "glove_link.h"
#include "glove_recording.h"
#include "imu_acquisition.h"
#include "max98357a_audio.h"
#include "wifi_transport.h"

static const char *TAG = "glove_imu";

static void report_battery_if_updated(void)
{
    static uint32_t last_sample_sequence;
    battery_status_t status;

    if (!battery_monitor_get_status(&status) ||
        status.sample_sequence == last_sample_sequence) {
        return;
    }
    last_sample_sequence = status.sample_sequence;

    printf("[BAT]%" PRId64 ",%s,%" PRIu32 ",%u\n",
           status.timestamp_ms, GLOVE_HAND_NAME, status.voltage_mv,
           (unsigned)status.percentage);
    fflush(stdout);

    esp_err_t ret = wifi_transport_send_battery(status.timestamp_ms,
                                                 status.voltage_mv,
                                                 status.percentage);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "发送电池状态失败: %s", esp_err_to_name(ret));
    }
}

static void print_csv_header(void)
{
    static const char *channel_names[IMU_MPU6050_COUNT] = {
        "f_0_s1", "f_0_s2", "f_1_s1", "f_1_s2", "f_2_s1",
        "f_2_s2", "f_3_s1", "f_3_s2", "f_4_s1", "f_4_s2",
    };

    printf("[DATA_HEAD]timestamp_ms,hand_type");
    for (int channel = 0; channel < IMU_MPU6050_COUNT; ++channel) {
        const char *name = channel_names[channel];
        printf(",%s_acc_x,%s_acc_y,%s_acc_z,%s_gyro_x,%s_gyro_y,%s_gyro_z",
               name, name, name, name, name, name);
    }
    printf(",dorsal_acc_x,dorsal_acc_y,dorsal_acc_z");
    printf(",dorsal_gyro_x,dorsal_gyro_y,dorsal_gyro_z");
    printf(",dorsal_mag_x,dorsal_mag_y,dorsal_mag_z\n");
}

/*
 * 一帧约 500 字节，放在静态缓冲区里一次写出；逐值 printf 会产生 70 次
 * 阻塞串口写，50 Hz 下会把采样节奏拖垮。
 */
static void print_raw_frame(const imu_frame_t *frame)
{
    static char line[640];
    size_t used = 0;

    int written = snprintf(line, sizeof(line), "[DATA]%" PRId64 ",%s",
                           frame->timestamp_ms, GLOVE_HAND_NAME);
    if (written < 0 || (size_t)written >= sizeof(line)) {
        return;
    }
    used = (size_t)written;

    for (int channel = 0; channel < IMU_MPU6050_COUNT; ++channel) {
        const imu_channel_data_t *data = &frame->channel[channel];
        written = snprintf(line + used, sizeof(line) - used,
                           ",%d,%d,%d,%d,%d,%d",
                           data->ax, data->ay, data->az,
                           data->gx, data->gy, data->gz);
        if (written < 0 || (size_t)written >= sizeof(line) - used) {
            return;
        }
        used += (size_t)written;
    }

    const imu_channel_data_t *dorsal =
        &frame->channel[IMU_ICM20948_INDEX];
    written = snprintf(line + used, sizeof(line) - used,
                       ",%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                       dorsal->ax, dorsal->ay, dorsal->az,
                       dorsal->gx, dorsal->gy, dorsal->gz,
                       dorsal->mx, dorsal->my, dorsal->mz);
    if (written < 0 || (size_t)written >= sizeof(line) - used) {
        return;
    }
    used += (size_t)written;

    fwrite(line, 1, used, stdout);
    fflush(stdout);
}

void app_main(void)
{
    printf("\n========== GLOVE FIRMWARE ID ==========\n");
    printf("HAND=%s\n", GLOVE_HAND_NAME_UPPER);
    printf("FIRMWARE_ID=%s\n", GLOVE_FIRMWARE_ID);
    printf("BUILD_DIRECTORY=%s\n", GLOVE_BUILD_DIRECTORY);
    printf("DATA_PORT=%d\n", GLOVE_ROCK_DATA_PORT);
    printf("=======================================\n\n");
    fflush(stdout);

    ESP_LOGI(TAG, "启动%s手11路IMU采集固件（hand_type=%s，不使用OLED）",
             GLOVE_HAND_NAME_CN, GLOVE_HAND_NAME);
    ESP_LOGI(TAG, "TCA1: GPIO%d(SDA), GPIO%d(SCL), CH0~CH7=8个MPU",
             TCA1_I2C_SDA_GPIO, TCA1_I2C_SCL_GPIO);
    ESP_LOGI(TAG, "TCA2: GPIO%d(SDA), GPIO%d(SCL), CH0~CH1=2个MPU, CH2=ICM",
             TCA2_I2C_SDA_GPIO, TCA2_I2C_SCL_GPIO);
#if CONFIG_GLOVE_HAND_LEFT
    ESP_LOGI(TAG, "按键: GPIO%d=开始/结束采集，GPIO%d=备用停止，GPIO%d=姿态校准",
             BUTTON_INFERENCE_GPIO, BUTTON_OPERATION_MODE_GPIO,
             BUTTON_CALIBRATION_GPIO);
#else
    ESP_LOGI(TAG, "按键: GPIO%d=工作模式，GPIO%d=姿态校准，GPIO%d为左手预留",
             BUTTON_OPERATION_MODE_GPIO, BUTTON_CALIBRATION_GPIO,
             BUTTON_INFERENCE_GPIO);
#endif

    /* 按键必须先于IMU初始化，便于在传感器未连接时独立测试按键。 */
    esp_err_t ret = glove_buttons_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败，继续尝试采集功能: %s",
                 esp_err_to_name(ret));
    }

    ret = battery_monitor_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "电池检测初始化失败，其他功能继续运行: %s",
                 esp_err_to_name(ret));
    }

#if CONFIG_GLOVE_HAND_LEFT
    /* 静音检查功放：初始化发送端，保持 SD/MODE 为低，不自动播放提示音。 */
    ret = max98357a_audio_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S发送端初始化失败，继续采集功能: %s",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "功放静音检查：SD/MODE保持低电平，不播放启动音；焊接状态需实测");
    }
#endif

    ret = wifi_transport_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi传输初始化失败，串口采集仍会继续: %s",
                 esp_err_to_name(ret));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(cloud_presence_init());

#if CONFIG_GLOVE_HAND_LEFT
    /* 左手是网关：上传自己 + 转发右手。 */
    ret = cloud_upload_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cloud upload init failed: %s", esp_err_to_name(ret));
    }
#endif

    /* ESP-NOW 必须在 esp_wifi_start() 之后初始化。 */
    ret = glove_link_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW左右手链路初始化失败: %s", esp_err_to_name(ret));
    }
#if CONFIG_GLOVE_HAND_LEFT
    else {
        glove_link_register_rx_cb(cloud_upload_submit_remote);
        ESP_LOGI(TAG, "网关模式：接收右手帧并转发到云端");
    }
#endif

    ret = imu_acquisition_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "没有可用的IMU，采集停止但按键测试继续: %s",
                 esp_err_to_name(ret));
        ESP_LOGW(TAG, "GPIO4/5/6按键任务继续运行；姿态校准需连接IMU后才能完成");
        while (true) {
            report_battery_if_updated();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    ESP_LOGI(TAG, "检测到%u/%d路IMU，请在零偏校准期间保持传感器静止",
             imu_get_detected_count(), IMU_TOTAL_CHANNELS);

    print_csv_header();

    TickType_t last_wakeup = xTaskGetTickCount();
#if CONFIG_GLOVE_HAND_LEFT
    uint32_t last_recording_generation = 0;
    int64_t last_recording_notice_ms = -1000;
#endif
    while (true) {
        imu_frame_t frame;
        ret = imu_acquisition_read_frame(&frame);
        if (ret == ESP_OK) {
            glove_buttons_process_imu_frame(&frame);
#if IMU_SERIAL_DATA_ENABLED
            /* 逐帧输出给上位机；每行一次写，控制台需使用 921600 波特率。 */
            print_raw_frame(&frame);
#endif
#if CONFIG_GLOVE_HAND_LEFT
            cloud_upload_submit(&frame);
#else
            /* 右手不在本机上传，交给左手网关转发。 */
            glove_link_send_frame(&frame);
#endif
            wifi_transport_send_frame(&frame);
        } else {
            ESP_LOGE(TAG, "读取IMU帧失败: %s", esp_err_to_name(ret));
        }

        report_battery_if_updated();

#if CONFIG_GLOVE_HAND_LEFT
        /*
         * 状态变化时立即通知右手；录制期间每秒一次心跳，右手 3 秒收不到
         * 就自动停止发送。停止时只发一次，右手漏包也会因心跳超时停下。
         */
        uint64_t recording_id = 0;
        bool recording = glove_recording_get(&recording_id);
        uint32_t generation = glove_recording_generation();
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (generation != last_recording_generation ||
            (recording && now_ms - last_recording_notice_ms >= 1000)) {
            glove_link_send_recording(recording, recording_id);
            last_recording_generation = generation;
            last_recording_notice_ms = now_ms;
        }
#endif

        vTaskDelayUntil(&last_wakeup,
                        pdMS_TO_TICKS(IMU_SAMPLE_INTERVAL_MS));
    }
}
