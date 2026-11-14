idf_component_register(
    SRCS
        "main.c"
        "serial_recording.c"
        "tca9548a.c"
        "mpu6050.c"
        "icm20948.c"
        "imu_acquisition.c"
        "glove_buttons.c"
        "wifi_transport.c"
        "wifi_portal.c"
        "cloud_upload.c"
        "cloud_presence.c"
        "glove_recording.c"
        "glove_link.c"
        "glove_sync.c"
        "imu_calibration.c"
        "max98357a_audio.c"
        "battery_monitor.c"
    INCLUDE_DIRS "."
    REQUIRES esp_adc esp_driver_gpio esp_driver_uart esp_driver_i2c esp_driver_i2s esp_timer esp_wifi esp_event esp_netif nvs_flash lwip esp_http_client mbedtls esp_psram esp_http_server spiffs espressif__cjson
)
spiffs_create_partition_image(wifi_web "${CMAKE_CURRENT_SOURCE_DIR}/wifi_web" FLASH_IN_PROJECT)
