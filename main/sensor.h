#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    float   temperature;   // degrees Celsius
    float   humidity;      // percent relative humidity
    int64_t timestamp_us;  // esp_timer value when the sample was taken
} sensor_reading_t;

/**
 * @brief Start the background task that polls the DHT22 sensor.
 *
 * @return ESP_OK on success, ESP_FAIL if the task could not be created.
 */
esp_err_t sensor_start(void);

/**
 * @brief Copy the most recent valid reading into the caller's buffer.
 *
 * @return true if a valid reading exists, false if the sensor has not
 *         produced one yet.
 */
bool sensor_get_latest(sensor_reading_t *out);
