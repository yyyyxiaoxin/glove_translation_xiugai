#include "cloud_presence.h"
#include <stdio.h>
#include <string.h>
#include "cloud_config.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "glove_identity.h"
#include "glove_recording.h"
#include "wifi_transport.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t cloud_presence_send(esp_http_client_handle_t client)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char url[160], body[160];
    snprintf(url, sizeof(url), "%s", GLOVE_CLOUD_URL);
    char *slash = strrchr(url, '/');
    if (!slash) return ESP_ERR_INVALID_ARG;
    snprintf(slash + 1, sizeof(url) - (slash + 1 - url), "heartbeat");
    snprintf(body, sizeof(body),
        "{\"device\":\"%02x%02x%02x%02x%02x%02x\",\"hand\":\"%s\",\"recording\":%s}",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], GLOVE_HAND_NAME,
        glove_recording_active() ? "true" : "false");
    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", "Bearer " GLOVE_CLOUD_TOKEN);
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t ret = esp_http_client_perform(client);
    if (ret == ESP_OK && esp_http_client_get_status_code(client) != 200) ret = ESP_FAIL;
    // A stale keep-alive socket can remain unusable after a server restart.
    // Drop it explicitly so the next heartbeat establishes a fresh TLS session.
    if (ret != ESP_OK) esp_http_client_close(client);
    esp_http_client_set_post_field(client, NULL, 0);
    esp_http_client_set_url(client, GLOVE_CLOUD_URL);
    return ret;
}

#if CONFIG_GLOVE_HAND_RIGHT
static void presence_task(void *arg)
{
    (void)arg;
    const esp_http_client_config_t config = {
        .url = GLOVE_CLOUD_URL, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000, .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { vTaskDelete(NULL); return; }
    for (;;) {
        if (wifi_transport_is_connected() && cloud_presence_send(client) != ESP_OK)
            ESP_LOGW("presence", "Heartbeat failed; retrying");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
#endif

esp_err_t cloud_presence_init(void)
{
#if CONFIG_GLOVE_HAND_RIGHT
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_err_t ret = esp_netif_sntp_init(&sntp);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    return xTaskCreate(presence_task, "cloud_presence", 8192, NULL, 1, NULL) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
#else
    return ESP_OK; /* Left reuses the upload client's TLS connection. */
#endif
}
