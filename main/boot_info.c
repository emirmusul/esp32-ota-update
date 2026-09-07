#include "boot_info.h"

#include <inttypes.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static const char *TAG = "boot_info";

const char *boot_info_state_str(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:            return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:          return "VALID";
    case ESP_OTA_IMG_INVALID:        return "INVALID";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:      return "UNDEFINED";
    default:                         return "UNKNOWN";
    }
}

void boot_info_log(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "version      : %s", desc->version);
    ESP_LOGI(TAG, "project      : %s", desc->project_name);
    ESP_LOGI(TAG, "built        : %s %s", desc->date, desc->time);
    ESP_LOGI(TAG, "idf          : %s", desc->idf_ver);

    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "running slot : %s @ 0x%06" PRIx32 " (size 0x%06" PRIx32 ")",
             running->label, running->address, running->size);

    const esp_partition_t *boot = esp_ota_get_boot_partition();
    ESP_LOGI(TAG, "boot slot    : %s", boot ? boot->label : "<none>");

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "next target  : %s @ 0x%06" PRIx32,
             next ? next->label : "<none>",
             next ? next->address : 0);

    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "image state  : %s", boot_info_state_str(state));
    } else {
        ESP_LOGI(TAG, "image state  : no otadata entry (%s)",
                 esp_err_to_name(err));
    }
}