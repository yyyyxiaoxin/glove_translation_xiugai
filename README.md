# 双手 IMU 手套与云端采集

ESP32-S3 左右手各采集 11 路 IMU。右手通过 ESP-NOW 向左手发送原始帧，左手配对后通过 HTTPS 上传。两手独立发送在线心跳。

## 固件

使用 ESP-IDF 6.1，分别构建左右手，避免混用配置：

```sh
idf.py -B build_left -D SDKCONFIG=sdkconfig.left -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.left build
idf.py -B build_right -D SDKCONFIG=sdkconfig.right -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.right build
idf.py -B build_left -p COM14 flash
idf.py -B build_right -p COM17 flash
```

首次构建前，将 `main/wifi_credentials.example.h` 复制为 `main/wifi_credentials.h` 并填写热点配置，将 `main/cloud_config.example.h` 复制为 `main/cloud_config.h` 并填写服务地址和上传口令。私密文件不会提交。

左手 GPIO4 切换开始/结束采集并播放中文提示，GPIO5 为备用停止。CH0–CH9 为指部 MPU6050，CH10 为掌部 ICM20948。网页使用保存的左手 CH0–CH7 六面标定系数展示初步校正效果；原始数据仍保留。

## 串口原始数据

左右手都按 50 Hz 逐帧打印一行：

```
[DATA]<timestamp_ms>,<left|right>,<IMU0 六轴>,...,<IMU9 六轴>,<IMU10 九轴>
```

共 2 + 69 个字段：本机毫秒时间戳、手别，随后 CH0–CH9 各 6 个值（ax,ay,az,gx,gy,gz），CH10 为 9 个值（含 mx,my,mz）。每行约 300–500 字节，50 Hz 约 20 KB/s，因此控制台使用 UART0（GPIO43 TX / GPIO44 RX）并配置为 **921600** 波特率（`CONFIG_ESP_CONSOLE_UART_CUSTOM` + `CONFIG_ESP_CONSOLE_UART_BAUDRATE`）。串口工具需设为 921600；ROM 启动信息仍以 115200 输出，所以上电最初的几个字节会显示乱码，属正常现象。将 `main/board_config.h` 的 `IMU_SERIAL_DATA_ENABLED` 设为 0 可关闭逐帧输出。

## 网页服务

`tools/imu_web/server.py` 使用 Python 标准库与 SQLite，部署在独立 `/imu/` 路径。配置 `IMU_TOKEN`（设备上传）、`IMU_ANNOTATION_TOKEN`（网页标注）、`IMU_DB`（数据库路径）。参考 `tools/imu_web/glove-imu.service`。

支持双手同时显示、在线心跳、记录标注及分类导出。CSV 默认按参考数据集导出 77 列：四项标签、sample_id、timestamp/ms、frame_id、hand_type，随后为 69 个原始寄存器值。缺失数据留空。`format=archive` 可导出调试归档字段。

```sh
python -m unittest discover -s tools/imu_web -p test_server.py
```

部分诊断和浏览器检查工具使用本机串口、参考数据路径或浏览器安装路径，运行前请调整。标定为手工六面初步结果；双手时间为软件估算配对，并非硬件同步。
