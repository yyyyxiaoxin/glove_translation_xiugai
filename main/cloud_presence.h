#pragma once
#include "esp_err.h"
#include "esp_http_client.h"
esp_err_t cloud_presence_send(esp_http_client_handle_t client);
esp_err_t cloud_presence_init(void);
