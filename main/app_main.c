#include "esp_log.h"
#include "nvs_flash.h"
#include "boot_info.h"

#include "wifi_sta.h"
#include "http_server.h"
#include "sensor.h"
#include "mdns_service.h"
#include "self_test.h"

static const char *TAG = "app_main";

void app_main(void)
{
    /* Initialise NVS: the WiFi driver stores calibration data and
       PHY settings there, so this must run before esp_wifi_init(). */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    boot_info_log();

    if (wifi_sta_init() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed, aborting");
        return;
    }

    // Not fatal: the board is still reachable by IP if mDNS fails.
    if (mdns_service_start() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS unavailable, reach the board by IP instead");
    }

    if (sensor_start() != ESP_OK) {
        ESP_LOGE(TAG, "Sensor task failed to start");
        return;
    }

    /* Updates are triggered on demand through POST /api/ota, not on boot.
       Starting a download automatically here would loop forever: every new
       image would boot, download, switch slots and restart again. */
    httpd_handle_t server = http_server_start();
    if (server == NULL) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return;
    }

    /* Runs last, once every subsystem has had a chance to come up. When the
       running image is on trial this call either confirms it or reboots
       into the previous slot, so nothing below it is guaranteed to run. */
    self_test_run();

    ESP_LOGI(TAG, "Setup complete");
}