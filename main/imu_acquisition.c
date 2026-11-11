#include "imu_acquisition.h"

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "board_config.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "icm20948.h"
#include "mpu6050.h"
#include "tca9548a.h"

#define MPU_FILTER_ALPHA          0.25f
#define ICM_FILTER_ALPHA          0.10f
#define MPU_GYRO_CALIB_SAMPLES    125
#define ICM_GYRO_CALIB_SAMPLES    250

typedef struct {
    int32_t sum[3];
    int16_t bias[3];
    uint16_t count;
    bool done;
} gyro_calibration_t;

typedef struct {
    float accel[3];
    float gyro[3];
    float mag[3];
    bool accel_initialized[3];
    bool gyro_initialized[3];
    bool mag_initialized[3];
} filter_state_t;

static const char *TAG = "imu_acquisition";

static i2c_master_bus_handle_t s_bus1;
static i2c_master_bus_handle_t s_bus2;
static tca9548a_handle_t s_mux1;
static tca9548a_handle_t s_mux2;
static mpu6050_handle_t s_mpu_bus1;
static mpu6050_handle_t s_mpu_bus2;
static icm20948_handle_t s_icm;

static bool s_mux1_ready;
static bool s_mux2_ready;
static bool s_detected[IMU_TOTAL_CHANNELS];
static uint32_t s_read_errors[IMU_TOTAL_CHANNELS];
static imu_channel_data_t s_latest[IMU_TOTAL_CHANNELS];
static gyro_calibration_t s_gyro_cal[IMU_TOTAL_CHANNELS];
static filter_state_t s_filter[IMU_TOTAL_CHANNELS];
static unsigned int s_detected_count;
static uint32_t s_sequence;
static bool s_initialized;

static esp_err_t create_bus(i2c_port_num_t port, gpio_num_t sda,
                            gpio_num_t scl,
                            i2c_master_bus_handle_t *bus_handle)
{
    const i2c_master_bus_config_t config = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&config, bus_handle);
}

static void mark_detected(int channel, bool has_mag)
{
    s_detected[channel] = true;
    s_latest[channel].valid = true;
    s_latest[channel].has_mag = has_mag;
    ++s_detected_count;
}

static void init_mpu_channels(tca9548a_handle_t *mux,
                              mpu6050_handle_t *mpu,
                              int channel_count, int global_offset,
                              const char *mux_name)
{
    for (int channel = 0; channel < channel_count; ++channel) {
        int global_channel = global_offset + channel;
        esp_err_t ret = tca9548a_select_channel(mux, (uint8_t)channel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "%s CH%d通道选择失败: %s", mux_name, channel,
                     esp_err_to_name(ret));
            continue;
        }

        uint8_t who = 0;
        ret = mpu6050_configure_selected(mpu, &who);
        if (ret == ESP_OK) {
            mark_detected(global_channel, false);
            ESP_LOGI(TAG, "IMU CH%d: %s下游CH%d，WHO_AM_I=0x%02X",
                     global_channel, mux_name, channel, who);
        } else {
            ESP_LOGW(TAG, "IMU CH%d: %s下游CH%d未检测到MPU: %s",
                     global_channel, mux_name, channel, esp_err_to_name(ret));
        }
    }
}

esp_err_t imu_acquisition_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    memset(s_detected, 0, sizeof(s_detected));
    memset(s_read_errors, 0, sizeof(s_read_errors));
    memset(s_latest, 0, sizeof(s_latest));
    memset(s_gyro_cal, 0, sizeof(s_gyro_cal));
    memset(s_filter, 0, sizeof(s_filter));
    s_detected_count = 0;
    s_sequence = 0;

    ESP_RETURN_ON_ERROR(create_bus(TCA1_I2C_PORT, TCA1_I2C_SDA_GPIO,
                                   TCA1_I2C_SCL_GPIO, &s_bus1),
                        TAG, "创建I2C0失败");
    ESP_RETURN_ON_ERROR(create_bus(TCA2_I2C_PORT, TCA2_I2C_SDA_GPIO,
                                   TCA2_I2C_SCL_GPIO, &s_bus2),
                        TAG, "创建I2C1失败");

    ESP_LOGI(TAG, "TCA1: I2C%d SDA=GPIO%d SCL=GPIO%d",
             TCA1_I2C_PORT, TCA1_I2C_SDA_GPIO, TCA1_I2C_SCL_GPIO);
    ESP_LOGI(TAG, "TCA2: I2C%d SDA=GPIO%d SCL=GPIO%d",
             TCA2_I2C_PORT, TCA2_I2C_SDA_GPIO, TCA2_I2C_SCL_GPIO);

    esp_err_t ret = tca9548a_init(&s_mux1, s_bus1,
                                  TCA9548A_DEFAULT_ADDRESS,
                                  GLOVE_I2C_FREQ_HZ);
    s_mux1_ready = ret == ESP_OK;
    if (!s_mux1_ready) {
        ESP_LOGE(TAG, "第一颗TCA9548A未检测到: %s", esp_err_to_name(ret));
    } else {
        ESP_RETURN_ON_ERROR(mpu6050_create(&s_mpu_bus1, s_bus1,
                                           MPU6050_DEFAULT_ADDRESS,
                                           GLOVE_I2C_FREQ_HZ),
                            TAG, "创建第一组MPU设备句柄失败");
        init_mpu_channels(&s_mux1, &s_mpu_bus1, TCA1_MPU_COUNT, 0, "TCA1");
        (void)tca9548a_disable_all_channels(&s_mux1);
    }

    ret = tca9548a_init(&s_mux2, s_bus2, TCA9548A_DEFAULT_ADDRESS,
                        GLOVE_I2C_FREQ_HZ);
    s_mux2_ready = ret == ESP_OK;
    if (!s_mux2_ready) {
        ESP_LOGE(TAG, "第二颗TCA9548A未检测到: %s", esp_err_to_name(ret));
    } else {
        ESP_RETURN_ON_ERROR(mpu6050_create(&s_mpu_bus2, s_bus2,
                                           MPU6050_DEFAULT_ADDRESS,
                                           GLOVE_I2C_FREQ_HZ),
                            TAG, "创建第二组MPU设备句柄失败");
        init_mpu_channels(&s_mux2, &s_mpu_bus2, TCA2_MPU_COUNT,
                          TCA1_MPU_COUNT, "TCA2");

        ESP_RETURN_ON_ERROR(icm20948_create(&s_icm, s_bus2,
                                            GLOVE_I2C_FREQ_HZ),
                            TAG, "创建ICM-20948设备句柄失败");
        ret = tca9548a_select_channel(&s_mux2, TCA2_ICM_CHANNEL);
        if (ret == ESP_OK) {
            ret = icm20948_configure_selected(&s_icm);
        }
        if (ret == ESP_OK) {
            mark_detected(IMU_ICM20948_INDEX, s_icm.mag_present);
            ESP_LOGI(TAG, "IMU CH10: TCA2下游CH%d，ICM-20948%s",
                     TCA2_ICM_CHANNEL,
                     s_icm.mag_present ? "九轴模式" : "六轴模式");
        } else {
            ESP_LOGW(TAG, "IMU CH10: 未检测到ICM-20948: %s",
                     esp_err_to_name(ret));
        }
        (void)tca9548a_disable_all_channels(&s_mux2);
    }

    ESP_LOGI(TAG, "初始化完成，检测到%u/%d路IMU",
             s_detected_count, IMU_TOTAL_CHANNELS);
    s_initialized = true;
    return s_detected_count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void update_gyro_calibration(int channel,
                                    int16_t gx, int16_t gy, int16_t gz)
{
    gyro_calibration_t *cal = &s_gyro_cal[channel];
    if (cal->done) {
        return;
    }

    cal->sum[0] += gx;
    cal->sum[1] += gy;
    cal->sum[2] += gz;
    ++cal->count;

    uint16_t required = channel == IMU_ICM20948_INDEX
                        ? ICM_GYRO_CALIB_SAMPLES
                        : MPU_GYRO_CALIB_SAMPLES;
    if (cal->count >= required) {
        cal->bias[0] = (int16_t)(cal->sum[0] / cal->count);
        cal->bias[1] = (int16_t)(cal->sum[1] / cal->count);
        cal->bias[2] = (int16_t)(cal->sum[2] / cal->count);
        cal->done = true;
        ESP_LOGI(TAG, "CH%d陀螺仪零偏完成: [%d,%d,%d]",
                 channel, cal->bias[0], cal->bias[1], cal->bias[2]);
    }
}

static void report_read_error(int channel, esp_err_t error)
{
    uint32_t count = ++s_read_errors[channel];
    if (count == 1 || (count % 50) == 0) {
        ESP_LOGW(TAG, "CH%d读取失败(累计%" PRIu32 "次): %s",
                 channel, count, esp_err_to_name(error));
    }
}

static void read_mpu_group(tca9548a_handle_t *mux,
                           mpu6050_handle_t *mpu,
                           int channel_count, int global_offset)
{
    for (int channel = 0; channel < channel_count; ++channel) {
        int global_channel = global_offset + channel;
        if (!s_detected[global_channel]) {
            continue;
        }

        esp_err_t ret = tca9548a_select_channel(mux, (uint8_t)channel);
        mpu6050_raw_data_t raw;
        if (ret == ESP_OK) {
            ret = mpu6050_read_raw(mpu, &raw);
        }
        if (ret != ESP_OK) {
            report_read_error(global_channel, ret);
            continue;
        }

        imu_channel_data_t *dst = &s_latest[global_channel];
        dst->valid = true;
        dst->ax = raw.ax;
        dst->ay = raw.ay;
        dst->az = raw.az;
        dst->gx = raw.gx;
        dst->gy = raw.gy;
        dst->gz = raw.gz;
        update_gyro_calibration(global_channel, raw.gx, raw.gy, raw.gz);
    }
}

esp_err_t imu_acquisition_read_frame(imu_frame_t *frame)
{
    ESP_RETURN_ON_FALSE(s_initialized && frame != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "采集模块未初始化");

    const int64_t scan_started_us = esp_timer_get_time();
    static uint64_t scan_sum_us;
    static uint32_t scan_max_us;
    static unsigned scan_count;

    for (int ch = 0; ch < IMU_TOTAL_CHANNELS; ++ch) {
        s_latest[ch].valid = false;
        s_latest[ch].has_mag = false;
    }

    if (s_mux1_ready) {
        read_mpu_group(&s_mux1, &s_mpu_bus1, TCA1_MPU_COUNT, 0);
    }
    if (s_mux2_ready) {
        read_mpu_group(&s_mux2, &s_mpu_bus2, TCA2_MPU_COUNT,
                       TCA1_MPU_COUNT);

        if (s_detected[IMU_ICM20948_INDEX]) {
            esp_err_t ret = tca9548a_select_channel(&s_mux2,
                                                    TCA2_ICM_CHANNEL);
            icm20948_raw_data_t raw;
            if (ret == ESP_OK) {
                ret = icm20948_read_raw(&s_icm, &raw);
            }
            if (ret == ESP_OK) {
                imu_channel_data_t *dst = &s_latest[IMU_ICM20948_INDEX];
                dst->valid = true;
                dst->has_mag = raw.mag_valid;
                dst->ax = raw.ax;
                dst->ay = raw.ay;
                dst->az = raw.az;
                dst->gx = raw.gx;
                dst->gy = raw.gy;
                dst->gz = raw.gz;
                if (raw.mag_valid) {
                    dst->mx = raw.mx;
                    dst->my = raw.my;
                    dst->mz = raw.mz;
                }
                update_gyro_calibration(IMU_ICM20948_INDEX,
                                        raw.gx, raw.gy, raw.gz);
            } else {
                report_read_error(IMU_ICM20948_INDEX, ret);
            }
        }
    }

    frame->sequence = s_sequence++;
    const int64_t scan_finished_us = esp_timer_get_time();
    const uint32_t scan_us = (uint32_t)(scan_finished_us - scan_started_us);
    /* Frame reference is the midpoint of the sequential read window.
     * This is not a hardware-synchronized sensor conversion timestamp. */
    frame->timestamp_ms = (scan_started_us + scan_finished_us) / 2000;
    scan_sum_us += scan_us;
    if (scan_us > scan_max_us) scan_max_us = scan_us;
    if (++scan_count == 250) {
        ESP_LOGI(TAG, "11-channel scan: mean=%" PRIu32 " us max=%" PRIu32 " us; target=%d ms",
                 (uint32_t)(scan_sum_us / scan_count), scan_max_us, IMU_SAMPLE_INTERVAL_MS);
        scan_count = 0;
        scan_sum_us = 0;
        scan_max_us = 0;
    }
    memcpy(frame->channel, s_latest, sizeof(frame->channel));
    return ESP_OK;
}

esp_err_t imu_get_channel_raw_data(int channel, imu_channel_data_t *out)
{
    ESP_RETURN_ON_FALSE(channel >= 0 && channel < IMU_TOTAL_CHANNELS &&
                        out != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "通道或输出参数无效");
    *out = s_latest[channel];
    return ESP_OK;
}

static int16_t clamp_i16(float value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static int16_t apply_filter(float *state, bool *initialized,
                            int16_t input, float alpha)
{
    if (!*initialized) {
        *state = input;
        *initialized = true;
    } else {
        *state += alpha * ((float)input - *state);
    }
    return clamp_i16(*state);
}

esp_err_t imu_get_channel_data(int channel, imu_channel_data_t *out)
{
    ESP_RETURN_ON_FALSE(channel >= 0 && channel < IMU_TOTAL_CHANNELS &&
                        out != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "通道或输出参数无效");

    const imu_channel_data_t *raw = &s_latest[channel];
    *out = *raw;
    if (!raw->valid) {
        return ESP_OK;
    }

    gyro_calibration_t *cal = &s_gyro_cal[channel];
    filter_state_t *filter = &s_filter[channel];
    float alpha = channel == IMU_ICM20948_INDEX
                  ? ICM_FILTER_ALPHA : MPU_FILTER_ALPHA;

    int32_t gx = raw->gx;
    int32_t gy = raw->gy;
    int32_t gz = raw->gz;
    if (cal->done) {
        gx -= cal->bias[0];
        gy -= cal->bias[1];
        gz -= cal->bias[2];
    }

    out->ax = apply_filter(&filter->accel[0], &filter->accel_initialized[0],
                           raw->ax, alpha);
    out->ay = apply_filter(&filter->accel[1], &filter->accel_initialized[1],
                           raw->ay, alpha);
    out->az = apply_filter(&filter->accel[2], &filter->accel_initialized[2],
                           raw->az, alpha);
    out->gx = apply_filter(&filter->gyro[0], &filter->gyro_initialized[0],
                           clamp_i16(gx), alpha);
    out->gy = apply_filter(&filter->gyro[1], &filter->gyro_initialized[1],
                           clamp_i16(gy), alpha);
    out->gz = apply_filter(&filter->gyro[2], &filter->gyro_initialized[2],
                           clamp_i16(gz), alpha);

    if (raw->has_mag) {
        out->mx = apply_filter(&filter->mag[0], &filter->mag_initialized[0],
                               raw->mx, alpha);
        out->my = apply_filter(&filter->mag[1], &filter->mag_initialized[1],
                               raw->my, alpha);
        out->mz = apply_filter(&filter->mag[2], &filter->mag_initialized[2],
                               raw->mz, alpha);
    }
    return ESP_OK;
}

unsigned int imu_get_detected_count(void)
{
    return s_detected_count;
}
