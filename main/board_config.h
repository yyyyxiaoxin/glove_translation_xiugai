#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"

/*
 * 两颗TCA9548A地址都为0x70，但分别连接ESP32-S3的两条I2C总线，
 * 因此不需要修改TCA模块上的A0/A1/A2焊盘。
 */
#define GLOVE_I2C_FREQ_HZ       400000

/* TCA9548A #1：8个MPU6050。 */
#define TCA1_I2C_PORT           I2C_NUM_0
#define TCA1_I2C_SDA_GPIO       GPIO_NUM_8
#define TCA1_I2C_SCL_GPIO       GPIO_NUM_9

/* TCA9548A #2：2个MPU6050和1个ICM-20948。 */
#define TCA2_I2C_PORT           I2C_NUM_1
#define TCA2_I2C_SDA_GPIO       GPIO_NUM_10
#define TCA2_I2C_SCL_GPIO       GPIO_NUM_11

#define TCA1_MPU_FIRST_CHANNEL  0
#define TCA1_MPU_COUNT          8

#define TCA2_MPU_FIRST_CHANNEL  0
#define TCA2_MPU_COUNT          2
#define TCA2_ICM_CHANNEL        2

#define IMU_SAMPLE_INTERVAL_MS  20

/*
 * 串口逐帧输出原始数据，供上位机采集：
 *   [DATA]<timestamp_ms>,<left|right>,<IMU0 六轴>,...,<IMU9 六轴>,<IMU10 九轴>\n
 * 每行约 400~500 字节，50 Hz 约 20~25 KB/s。控制台波特率必须同步改为
 * 921600（sdkconfig 中 CONFIG_ESP_CONSOLE_UART_BAUDRATE）；
 * 115200 下一行要写 40 ms 以上，采样会掉到 10 Hz 左右。
 */
#define IMU_SERIAL_DATA_ENABLED   1
#define IMU_SERIAL_DATA_BAUDRATE  921600

/* Optional legacy LAN stream; cloud recording uses ESP-NOW and HTTPS. */
#define GLOVE_LEGACY_UDP_ENABLED  0

/*
 * 手套业务按键：GPIO内部上拉，按键另一端接GND，低电平有效。
 * GPIO4仅在左手固件中启用；GPIO5、GPIO6在左右手固件中都启用。
 * GPIO0保留给BOOT下载键，EN保留给复位键，不与业务按键复用。
 */
#define BUTTON_INFERENCE_GPIO       GPIO_NUM_4
#define BUTTON_OPERATION_MODE_GPIO  GPIO_NUM_5
#define BUTTON_CALIBRATION_GPIO     GPIO_NUM_6

/* 左手 MAX98357A I2S 功放：SD/MODE 拉高启用，拉低静音。 */
#define MAX98357A_BCLK_GPIO         GPIO_NUM_12
#define MAX98357A_LRCLK_GPIO        GPIO_NUM_13
#define MAX98357A_DIN_GPIO          GPIO_NUM_14
#define MAX98357A_SD_GPIO           GPIO_NUM_15

/*
 * 锂电池电压检测：电池正极经等值电阻分压后接 GPIO1/ADC1_CH0。
 * 例如 BAT+ -- 100kΩ -- GPIO1 -- 100kΩ -- GND，GPIO1 对地并联 100nF。
 * 分压后的最高电压必须低于 3.3V，禁止把电池正极直接接入 GPIO1。
 */
#define BATTERY_ADC_GPIO                GPIO_NUM_1
#define BATTERY_DIVIDER_NUMERATOR       2U
#define BATTERY_DIVIDER_DENOMINATOR     1U
#define BATTERY_FULL_VOLTAGE_MV         4200U
#define BATTERY_EMPTY_VOLTAGE_MV        3000U
#define BATTERY_SAMPLE_INTERVAL_MS      1000U
#define BATTERY_FILTER_SIZE             8U

#define BUTTON_POLL_INTERVAL_MS     20
#define BUTTON_DEBOUNCE_MS          60

/* 姿态阈值校准参数，与原项目保持一致。 */
#define CALIBRATION_WAIT_MS             2000
#define CALIBRATION_SAMPLE_INTERVAL_MS  100
#define CALIBRATION_SAMPLE_COUNT        50
#define CALIBRATION_MARGIN              2000
