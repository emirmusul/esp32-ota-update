#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "dht.h"
#include "sensor.h"

static const char *TAG = "sensor";

static sensor_reading_t  s_latest;
static bool              s_has_reading = false;
static SemaphoreHandle_t s_mutex       = NULL;

static void sensor_task(void *arg)
{
    const TickType_t interval = pdMS_TO_TICKS(CONFIG_DHT_INTERVAL_MS);
    uint32_t failures = 0;

    // The sensor needs about a second after power-up before it answers.
    vTaskDelay(pdMS_TO_TICKS(1500));

    for (;;) {
        dht_reading_t raw;
        esp_err_t err = dht_read(CONFIG_DHT_GPIO, &raw);

        if (err == ESP_OK) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_latest.temperature  = raw.temperature;
            s_latest.humidity     = raw.humidity;
            s_latest.timestamp_us = esp_timer_get_time();
            s_has_reading         = true;
            xSemaphoreGive(s_mutex);

            if (failures > 0) {
                ESP_LOGI(TAG, "Recovered after %lu failed read(s)",
                         (unsigned long) failures);
                failures = 0;
            }

            ESP_LOGI(TAG, "%.1f C, %.1f %%", raw.temperature, raw.humidity);
        } else {
            failures++;
            ESP_LOGW(TAG, "Read failed (%s), %lu consecutive failure(s)",
                     esp_err_to_name(err), (unsigned long) failures);
        }

        vTaskDelay(interval);
    }
}

esp_err_t sensor_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }

    // Pinned to core 1 because the WiFi driver runs on core 0. Bit-banging the
    // DHT22 depends on microsecond timing, so keeping it away from the busiest
    // core reduces the chance of being preempted mid-transfer.
    BaseType_t ok = xTaskCreatePinnedToCore(sensor_task, "sensor", 3072,
                                            NULL, 5, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sensor task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sensor task started on GPIO %d, interval %d ms",
             CONFIG_DHT_GPIO, CONFIG_DHT_INTERVAL_MS);
    return ESP_OK;
}

bool sensor_get_latest(sensor_reading_t *out)
{
    bool valid;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    valid = s_has_reading;
    if (valid) {
        *out = s_latest;
    }
    xSemaphoreGive(s_mutex);

    return valid;
}
