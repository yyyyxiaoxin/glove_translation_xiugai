# 扬声器独立测试固件

目标芯片为 ESP32-S3，复用主工程的 MAX98357A 驱动和板级引脚配置。

- BCLK：GPIO12
- LRCLK：GPIO13
- DIN：GPIO14
- SD/MODE：GPIO15

启动后等待约 2 秒，以约 3 秒为周期播放 0.5 秒的 1 kHz 低音量提示音。
串口为 115200 波特率，固件标识为 `GLOVE_SPEAKER_TEST`。
本固件只启动音频测试，不运行手套传感器采集或 Wi-Fi。

在已配置的 ESP-IDF 5.5 环境中，从本目录执行：

```powershell
idf.py build
idf.py -p COM12 flash monitor
```

按实际串口修改 COM12。烧录会将板上应用替换为本测试程序。
扬声器接功放 OUT+ 和 OUT−，不要将任一扬声器输出接地。
日志中播放完成代表 I2S 发送函数返回成功，是否真正发声需听音确认。
