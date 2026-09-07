#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "ota.h"

static const char *TAG = "ota";

#include "freertos/semphr.h"

/* Guards against a second update starting while one is in flight.
   Two concurrent esp_ota_begin() calls on the same partition would
   fail with ESP_ERR_OTA_PARTITION_CONFLICT, and the HTTP endpoint is
   trivially easy to hit twice. */
static volatile bool s_update_in_progress = false;

#define OTA_BUF_SIZE 1024

esp_err_t ota_download_to_inactive_slot(const char *url)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        ESP_LOGE(TAG, "No OTA partition available");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Target slot: %s @ 0x%06lx", target->label,
             (unsigned long) target->address);

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .timeout_ms        = CONFIG_OTA_RECV_TIMEOUT_MS,
        .keep_alive_enable = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot reach %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    /* Reads the status line and headers without consuming the body, so
       Content-Length is known before the erase begins. */
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status != 200) {
        ESP_LOGE(TAG, "Server returned HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    if (content_length <= 0) {
        ESP_LOGE(TAG, "Server did not report a body size");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    if (content_length > target->size) {
        ESP_LOGE(TAG, "Image is %d bytes, slot holds %lu",
                 content_length, (unsigned long) target->size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Image size %d bytes, erasing...", content_length);

    /* Passing the exact size erases only the sectors that will be used.
       OTA_SIZE_UNKNOWN would erase the full 1.9 MB and take far longer. */
    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(target, content_length, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    ESP_LOGI(TAG, "Erase done, downloading...");

    char *buf = malloc(OTA_BUF_SIZE);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Out of memory");
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int written = 0;
    int last_logged_pct = -1;

    while (written < content_length) {
        int n = esp_http_client_read(client, buf, OTA_BUF_SIZE);

        if (n < 0) {
            ESP_LOGE(TAG, "Read error after %d bytes", written);
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            /* Connection closed before Content-Length was satisfied. */
            ESP_LOGE(TAG, "Connection closed early: %d of %d bytes",
                     written, content_length);
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        err = esp_ota_write(ota_handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at %d bytes: %s",
                     written, esp_err_to_name(err));
            break;
        }

        written += n;

        int pct = (written * 100) / content_length;
        if (pct / 10 != last_logged_pct / 10) {
            ESP_LOGI(TAG, "%d%% (%d/%d)", pct, written, content_length);
            last_logged_pct = pct;
        }
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        esp_ota_abort(ota_handle);
        return err;
    }

    /* Computes SHA256 over what was written and compares it with the
       digest appended to the image at build time. */
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image failed verification (corrupt download)");
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    ESP_LOGI(TAG, "Image written and verified");

    /* Read the descriptor back out of flash: proof that a real, valid
       application now lives in the inactive slot. */
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(target, &desc) == ESP_OK) {
        ESP_LOGI(TAG, "Slot %s now holds: %s v%s (built %s %s)",
                 target->label, desc.project_name, desc.version,
                 desc.date, desc.time);
    }

    return ESP_OK;
}

esp_err_t ota_update_and_reboot(const char *url)
{
    if (url == NULL) {
        url = CONFIG_OTA_UPDATE_URL;
    }

    esp_err_t err = ota_download_to_inactive_slot(url);
    if (err != ESP_OK) {
        return err;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);

    /* Writes a new ota_seq into whichever otadata sector is currently
       unused. The active sector stays intact until the write completes,
       so a power cut here leaves the old slot selected. The target image
       is re-verified before the sequence number is bumped. */
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "Boot partition set to %s, restarting in 1 s",
             target->label);

    /* Gives the HTTP response time to reach the client and the UART
       buffer time to drain before the reset. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;  /* unreachable */
}

static void ota_update_task(void *arg)
{
    esp_err_t err = ota_update_and_reboot(NULL);

    /* Only reached on failure: esp_restart() does not return. */
    ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(err));
    s_update_in_progress = false;
    vTaskDelete(NULL);
}

esp_err_t ota_trigger_async(void)
{
    if (s_update_in_progress) {
        ESP_LOGW(TAG, "Update already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    s_update_in_progress = true;

    /* Priority 4 keeps this below the sensor task (5) and far below the
       WiFi driver (23). Flash erase blocks for hundreds of milliseconds,
       so this must not be the highest-priority runnable task. */
    BaseType_t ok = xTaskCreate(ota_update_task, "ota_update", 8192,
                                NULL, 4, NULL);
    if (ok != pdPASS) {
        s_update_in_progress = false;
        ESP_LOGE(TAG, "Failed to create OTA task");
        return ESP_FAIL;
    }

    return ESP_OK;
}
