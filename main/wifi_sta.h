#pragma once

#include "esp_err.h"

/**
 * @brief Initialise WiFi in station mode and block until an IP is acquired.
 *
 * @return ESP_OK on success, ESP_FAIL if all retries were exhausted.
 */
esp_err_t wifi_sta_init(void);