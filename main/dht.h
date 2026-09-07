#pragma once
#include "esp_err.h"
#include "driver/gpio.h"

typedef struct {
    float temperature;
    float humidity;
} dht_reading_t;

/**
 * Reads temperature and humidity from a DHT22 sensor.
 * @param pin GPIO pin connected to the DHT22 data line
 * @param out pointer to a dht_reading_t struct to fill with results
 * @return ESP_OK on success, ESP_ERR_TIMEOUT on communication timeout,
 *         ESP_ERR_INVALID_CRC on checksum mismatch
 */
esp_err_t dht_read(gpio_num_t pin, dht_reading_t *out);