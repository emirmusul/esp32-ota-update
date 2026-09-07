#include "esp_log.h"
#include "mdns.h"

#include "mdns_service.h"

static const char *TAG = "mdns_service";

esp_err_t mdns_service_start(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Claiming a name is not instant: the stack probes the link three times to
    // make sure nobody else answers to it, then announces itself. If the name
    // is taken it falls back to <hostname>-2.local on its own.
    err = mdns_hostname_set(CONFIG_MDNS_HOSTNAME);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_instance_name_set("ESP32 Sensor Dashboard");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(err));
        return err;
    }

    // DNS-SD advertisement on top of mDNS, so the board shows up in service
    // browsers such as "dns-sd -B _http._tcp" on macOS.
    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Reachable at http://%s.local", CONFIG_MDNS_HOSTNAME);
    return ESP_OK;
}
