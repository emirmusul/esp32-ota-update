#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"

#include "self_test.h"
#include "sensor.h"

static const char *TAG = "self_test";

/* Polling interval while waiting for the first sensor reading. */
#define SELF_TEST_POLL_MS 250

/* A station that reached the RUN state and holds an IP is reachable, which
   is the property that matters: without it there is no way to push a fixing
   image and the board is effectively lost. */
static bool check_wifi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        ESP_LOGE(TAG, "Not associated with an access point");
        return false;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;

    if (netif == NULL || esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
        ESP_LOGE(TAG, "No netif handle for the station interface");
        return false;
    }

    if (ip.ip.addr == 0) {
        ESP_LOGE(TAG, "Associated with %s but no IP assigned", ap.ssid);
        return false;
    }

    ESP_LOGI(TAG, "WiFi OK: %s, rssi %d, ip " IPSTR,
             ap.ssid, ap.rssi, IP2STR(&ip.ip));
    return true;
}

/* The sensor task starts with a 1.5 s delay and individual reads fail
   occasionally, so a single sample proves nothing. Poll until one lands or
   the window closes. */
static bool check_sensor(void)
{
    const int deadline = CONFIG_SELF_TEST_TIMEOUT_MS / SELF_TEST_POLL_MS;
    sensor_reading_t reading;

    for (int i = 0; i < deadline; i++) {
        if (sensor_get_latest(&reading)) {
            ESP_LOGI(TAG, "Sensor OK: %.1f C, %.1f %% after %d ms",
                     reading.temperature, reading.humidity,
                     i * SELF_TEST_POLL_MS);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SELF_TEST_POLL_MS));
    }

    ESP_LOGE(TAG, "No sensor reading within %d ms",
             CONFIG_SELF_TEST_TIMEOUT_MS);
    return false;
}

bool self_test_run(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();

    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);

    /* PENDING_VERIFY means the bootloader started this image for the first
       time and is waiting to be told whether it works. Any reset before
       that decision is taken counts as a failure and reverts the board. */
    const bool rollback_pending =
        (err == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY);

    if (rollback_pending) {
        ESP_LOGW(TAG, "Image is PENDING_VERIFY — this boot decides its fate");
    }

    const bool wifi_ok   = check_wifi();
    const bool sensor_ok = check_sensor();
    const bool passed    = wifi_ok && sensor_ok;

    if (!rollback_pending) {
        ESP_LOGI(TAG, "Health check %s (no rollback pending)",
                 passed ? "passed" : "FAILED");
        return passed;
    }

    if (passed) {
        /* Writes VALID into otadata. Without this call the bootloader would
           revert to the other slot on the next reset, however long the board
           has been running. */
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Health check passed, image marked valid");
        } else {
            ESP_LOGE(TAG, "mark_app_valid failed: %s", esp_err_to_name(err));
        }
        return true;
    }

    ESP_LOGE(TAG, "Health check FAILED — rolling back to the other slot");

    /* Marks this image INVALID so it is never selected again, then reboots.
       Does not return. */
    err = esp_ota_mark_app_invalid_rollback_and_reboot();

    /* Only reached if there is no other valid image to fall back to. */
    ESP_LOGE(TAG, "Rollback impossible: %s", esp_err_to_name(err));
    return false;
}