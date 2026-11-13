idf_component_register(
    SRCS
        "main.c"
        "tca9548a.c"
        "mpu6050.c"
        "icm20948.c"
        "imu_acquisition.c"
        "glove_buttons.c"
        "wifi_transport.c"
        "cloud_upload.c"
        "cloud_presence.c"
        "glove_recording.c"
        "glove_link.c"
        "glove_sync.c"
        "imu_calibration.c"
        "max98357a_audio.c"
        "battery_monitor.c"
    INCLUDE_DIRS "."
    REQUIRES esp_adc esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_timer esp_wifi esp_event esp_netif nvs_flash lwip esp_http_client mbedtls esp_psram
)
