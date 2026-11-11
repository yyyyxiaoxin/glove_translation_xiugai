#include "wifi_transport.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "glove_identity.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "wifi_credentials.h"

#define UDP_FRAME_BUFFER_SIZE  1024

static const char *TAG = "wifi_transport";

static int s_udp_socket = -1;
static struct sockaddr_in s_destination;
static volatile bool s_connected;
static bool s_hello_sent;
static uint32_t s_sent_frames;

static esp_err_t append_text(char *buffer, size_t capacity, size_t *length,
                             const char *format, ...)
{
    if (*length >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }

    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer + *length, capacity - *length, format, args);
    va_end(args);

    if (written < 0 || (size_t)written >= capacity - *length) {
        return ESP_ERR_INVALID_SIZE;
    }

    *length += (size_t)written;
    return ESP_OK;
}

static esp_err_t create_udp_socket(void)
{
    s_udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_udp_socket < 0) {
        ESP_LOGE(TAG, "创建UDP套接字失败: errno=%d", errno);
        return ESP_FAIL;
    }

    int enable_broadcast = 1;
    if (setsockopt(s_udp_socket, SOL_SOCKET, SO_BROADCAST,
                   &enable_broadcast, sizeof(enable_broadcast)) < 0) {
        ESP_LOGE(TAG, "启用UDP广播失败: errno=%d", errno);
        close(s_udp_socket);
        s_udp_socket = -1;
        return ESP_FAIL;
    }

    memset(&s_destination, 0, sizeof(s_destination));
    s_destination.sin_family = AF_INET;
    s_destination.sin_port = htons(GLOVE_ROCK_DATA_PORT);
    s_destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    return ESP_OK;
}

static void send_hello_if_needed(void)
{
    if (s_hello_sent) {
        return;
    }

    char hello[96];
    int hello_length = snprintf(
        hello, sizeof(hello), "[HELLO]%s,%s,%d\n",
        GLOVE_FIRMWARE_ID, GLOVE_HAND_NAME, GLOVE_ROCK_DATA_PORT);
    if (hello_length > 0 && (size_t)hello_length < sizeof(hello)) {
        sendto(s_udp_socket, hello, (size_t)hello_length, 0,
               (struct sockaddr *)&s_destination, sizeof(s_destination));
    }
    s_hello_sent = true;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "正在连接热点 %s", GLOVE_WIFI_SSID);
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        s_connected = false;
        s_hello_sent = false;
        ESP_LOGW(TAG, "Wi-Fi已断开(reason=%u)，正在自动重连", event->reason);
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        const uint32_t broadcast_addr =
            event->ip_info.ip.addr | ~event->ip_info.netmask.addr;
        s_destination.sin_addr.s_addr = broadcast_addr;
        s_hello_sent = false;
        s_connected = true;

        esp_ip4_addr_t broadcast_ip = { .addr = broadcast_addr };
        ESP_LOGI(TAG, "Wi-Fi已连接，ESP32-S3 IP=" IPSTR,
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "IMU数据通过UDP广播到 " IPSTR ":%d",
                 IP2STR(&broadcast_ip), GLOVE_ROCK_DATA_PORT);
    }
}

esp_err_t wifi_transport_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS需要重新初始化");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "擦除NVS失败");
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "初始化NVS失败");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "初始化网络接口失败");
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "创建事件循环失败");
    }

    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "创建Wi-Fi STA接口失败");
        return ESP_FAIL;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "初始化Wi-Fi失败");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   &wifi_event_handler, NULL),
        TAG, "注册Wi-Fi事件失败");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   &wifi_event_handler, NULL),
        TAG, "注册IP事件失败");

    wifi_config_t wifi_config = { 0 };
    snprintf((char *)wifi_config.sta.ssid,
             sizeof(wifi_config.sta.ssid), "%s", GLOVE_WIFI_SSID);
    snprintf((char *)wifi_config.sta.password,
             sizeof(wifi_config.sta.password), "%s", GLOVE_WIFI_PASSWORD);
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM),
                        TAG, "设置Wi-Fi存储模式失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA),
                        TAG, "设置Wi-Fi STA模式失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config),
                        TAG, "写入热点配置失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE),
                        TAG, "关闭Wi-Fi省电失败");
    ESP_RETURN_ON_ERROR(create_udp_socket(), TAG, "初始化UDP发送失败");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "启动Wi-Fi失败");

    ESP_LOGI(TAG, "局域网传输已启动：SSID=%s，端口=%d（密码不打印）",
             GLOVE_WIFI_SSID, GLOVE_ROCK_DATA_PORT);
    return ESP_OK;
}

bool wifi_transport_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_transport_send_frame(const imu_frame_t *frame)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_connected || s_udp_socket < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    send_hello_if_needed();

    char packet[UDP_FRAME_BUFFER_SIZE];
    size_t length = 0;
    esp_err_t ret = append_text(packet, sizeof(packet), &length,
                                "[DATA]%" PRId64 ",%s,%" PRIu32,
                                frame->timestamp_ms, GLOVE_HAND_NAME,
                                frame->sequence);
    if (ret != ESP_OK) {
        return ret;
    }

    for (int channel = 0; channel < IMU_MPU6050_COUNT; ++channel) {
        const imu_channel_data_t *data = &frame->channel[channel];
        ret = append_text(packet, sizeof(packet), &length,
                          ",%d,%d,%d,%d,%d,%d",
                          data->ax, data->ay, data->az,
                          data->gx, data->gy, data->gz);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    const imu_channel_data_t *dorsal =
        &frame->channel[IMU_ICM20948_INDEX];
    ret = append_text(packet, sizeof(packet), &length,
                      ",%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                      dorsal->ax, dorsal->ay, dorsal->az,
                      dorsal->gx, dorsal->gy, dorsal->gz,
                      dorsal->mx, dorsal->my, dorsal->mz);
    if (ret != ESP_OK) {
        return ret;
    }

    int sent = sendto(s_udp_socket, packet, length, 0,
                      (struct sockaddr *)&s_destination,
                      sizeof(s_destination));
    if (sent < 0) {
        ESP_LOGE(TAG, "UDP发送失败: errno=%d", errno);
        return ESP_FAIL;
    }
    if ((size_t)sent != length) {
        ESP_LOGW(TAG, "UDP数据包未完整发送: %d/%u", sent, (unsigned)length);
        return ESP_FAIL;
    }

    ++s_sent_frames;
    if (s_sent_frames == 1 || (s_sent_frames % 100) == 0) {
        ESP_LOGI(TAG, "Wi-Fi已发送%" PRIu32 "帧，当前sequence=%" PRIu32,
                 s_sent_frames, frame->sequence);
    }
    return ESP_OK;
}

esp_err_t wifi_transport_send_battery(int64_t timestamp_ms,
                                      uint32_t voltage_mv,
                                      uint8_t percentage)
{
    if (!s_connected || s_udp_socket < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    send_hello_if_needed();

    char packet[96];
    int length = snprintf(packet, sizeof(packet),
                          "[BAT]%" PRId64 ",%s,%" PRIu32 ",%u\n",
                          timestamp_ms, GLOVE_HAND_NAME, voltage_mv,
                          (unsigned)percentage);
    if (length < 0 || (size_t)length >= sizeof(packet)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int sent = sendto(s_udp_socket, packet, (size_t)length, 0,
                      (struct sockaddr *)&s_destination,
                      sizeof(s_destination));
    if (sent < 0) {
        ESP_LOGE(TAG, "UDP电池状态发送失败: errno=%d", errno);
        return ESP_FAIL;
    }
    if (sent != length) {
        ESP_LOGW(TAG, "UDP电池状态未完整发送: %d/%d", sent, length);
        return ESP_FAIL;
    }

    return ESP_OK;
}
