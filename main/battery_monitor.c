#include "battery_monitor.h"

#include <inttypes.h>
#include <stddef.h>

#include "board_config.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BATTERY_ADC_UNIT             ADC_UNIT_1
#define BATTERY_ADC_CHANNEL          ADC_CHANNEL_0
#define BATTERY_ADC_ATTENUATION      ADC_ATTEN_DB_12
#define BATTERY_ADC_BITWIDTH         ADC_BITWIDTH_12
#define BATTERY_ADC_RESOLUTION       4096U
#define BATTERY_ADC_REFERENCE_MV     3300U
#define BATTERY_TASK_STACK_SIZE      3072
#define BATTERY_TASK_PRIORITY        5
#define BATTERY_STARTUP_DELAY_MS     500

static const char *TAG = "battery_monitor";

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_calibration_handle;
static bool s_calibration_enabled;
static battery_status_t s_status;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;

static uint8_t voltage_to_percentage(uint32_t voltage_mv)
{
    if (voltage_mv >= BATTERY_FULL_VOLTAGE_MV) {
        return 100;
    }
    if (voltage_mv <= BATTERY_EMPTY_VOLTAGE_MV) {
        return 0;
    }

    return (uint8_t)(((voltage_mv - BATTERY_EMPTY_VOLTAGE_MV) * 100U) /
                     (BATTERY_FULL_VOLTAGE_MV -
                      BATTERY_EMPTY_VOLTAGE_MV));
}

static esp_err_t raw_to_battery_voltage(int raw_adc, uint32_t *voltage_mv)
{
    if (voltage_mv == NULL || raw_adc < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int adc_mv = 0;
    if (s_calibration_enabled) {
        esp_err_t ret = adc_cali_raw_to_voltage(s_calibration_handle,
                                                raw_adc, &adc_mv);
        if (ret != ESP_OK) {
            return ret;
        }
    } else {
        /* 与原 ART-Pi2 程序相同的 12 位、3.3 V 近似换算作为后备。 */
        adc_mv = (int)(((uint32_t)raw_adc * BATTERY_ADC_REFERENCE_MV) /
                       BATTERY_ADC_RESOLUTION);
    }

    *voltage_mv = ((uint32_t)adc_mv * BATTERY_DIVIDER_NUMERATOR +
                   (BATTERY_DIVIDER_DENOMINATOR / 2U)) /
                  BATTERY_DIVIDER_DENOMINATOR;
    return ESP_OK;
}

static bool init_calibration(void)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t config = {
        .unit_id = BATTERY_ADC_UNIT,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = BATTERY_ADC_ATTENUATION,
        .bitwidth = BATTERY_ADC_BITWIDTH,
    };

    esp_err_t ret = adc_cali_create_scheme_curve_fitting(
        &config, &s_calibration_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC 曲线拟合校准已启用");
        return true;
    }

    ESP_LOGW(TAG, "ADC 校准不可用，将使用 3.3 V 近似换算: %s",
             esp_err_to_name(ret));
#else
    ESP_LOGW(TAG, "芯片未提供 ADC 曲线拟合校准，将使用 3.3 V 近似换算");
#endif
    return false;
}

static void battery_task(void *argument)
{
    (void)argument;

    int filter[BATTERY_FILTER_SIZE] = {0};
    size_t filter_index = 0;
    size_t filter_count = 0;
    uint32_t sample_sequence = 0;

    vTaskDelay(pdMS_TO_TICKS(BATTERY_STARTUP_DELAY_MS));

    while (true) {
        int raw_adc = 0;
        esp_err_t ret = adc_oneshot_read(s_adc_handle,
                                         BATTERY_ADC_CHANNEL, &raw_adc);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "读取电池 ADC 失败: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(BATTERY_SAMPLE_INTERVAL_MS));
            continue;
        }

        filter[filter_index] = raw_adc;
        filter_index = (filter_index + 1U) % BATTERY_FILTER_SIZE;
        if (filter_count < BATTERY_FILTER_SIZE) {
            ++filter_count;
        }

        uint32_t raw_sum = 0;
        for (size_t i = 0; i < filter_count; ++i) {
            raw_sum += (uint32_t)filter[i];
        }
        int average_raw = (int)(raw_sum / filter_count);

        uint32_t voltage_mv = 0;
        ret = raw_to_battery_voltage(average_raw, &voltage_mv);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "换算电池电压失败: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(BATTERY_SAMPLE_INTERVAL_MS));
            continue;
        }

        battery_status_t next = {
            .raw_adc = average_raw,
            .voltage_mv = voltage_mv,
            .percentage = voltage_to_percentage(voltage_mv),
            .timestamp_ms = esp_timer_get_time() / 1000,
            .sample_sequence = ++sample_sequence,
            .valid = true,
        };

        portENTER_CRITICAL(&s_status_lock);
        s_status = next;
        portEXIT_CRITICAL(&s_status_lock);

        vTaskDelay(pdMS_TO_TICKS(BATTERY_SAMPLE_INTERVAL_MS));
    }
}

esp_err_t battery_monitor_init(void)
{
    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = BATTERY_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t ret = adc_oneshot_new_unit(&unit_config, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化 ADC1 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = BATTERY_ADC_ATTENUATION,
        .bitwidth = BATTERY_ADC_BITWIDTH,
    };
    ret = adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CHANNEL,
                                     &channel_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "配置 ADC1_CH0 失败: %s", esp_err_to_name(ret));
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return ret;
    }

    s_calibration_enabled = init_calibration();

    BaseType_t task_created = xTaskCreate(battery_task, "battery_adc",
                                          BATTERY_TASK_STACK_SIZE, NULL,
                                          BATTERY_TASK_PRIORITY, NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "创建电池采样任务失败");
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        if (s_calibration_enabled) {
            adc_cali_delete_scheme_curve_fitting(s_calibration_handle);
            s_calibration_handle = NULL;
            s_calibration_enabled = false;
        }
#endif
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "电池检测已启动: GPIO%d/ADC1_CH0, 分压比=%u:%u, "
             "1秒采样, %u点滑动平均",
             BATTERY_ADC_GPIO, BATTERY_DIVIDER_NUMERATOR,
             BATTERY_DIVIDER_DENOMINATOR, BATTERY_FILTER_SIZE);
    return ESP_OK;
}

bool battery_monitor_get_status(battery_status_t *status)
{
    if (status == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_status_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    return status->valid;
}
