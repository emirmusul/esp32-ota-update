#pragma once

#include "esp_err.h"

/**
 * @brief Advertise the board as <hostname>.local and register an HTTP service.
 *
 * @return ESP_OK on success, an esp_err_t code from the mDNS stack otherwise.
 */
esp_err_t mdns_service_start(void);
