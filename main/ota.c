#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "ota.h"

static const char *TAG = "ota";

#define OTA_BUF_SIZE 1024

/* The application descriptor sits at a fixed offset in every ESP-IDF image,
   right after the image header and the first segment header. Reading this
   many bytes is enough to learn the version without downloading the rest. */
#define OTA_DESC_OFFSET (sizeof(esp_image_header_t) + \
                         sizeof(esp_image_segment_header_t))
#define OTA_HEADER_BYTES (OTA_DESC_OFFSET + sizeof(esp_app_desc_t))

/* Guards against a second update starting while one is in flight.
   Two concurrent esp_ota_begin() calls on the same partition would
   fail with ESP_ERR_OTA_PARTITION_CONFLICT, and the HTTP endpoint is
   trivially easy to hit twice. */
static volatile bool s_update_in_progress = false;

/* esp_http_client_read() returns whatever the socket has available, which
   follows TCP segment boundaries rather than anything we asked for. Loop
   until the requested count is in hand or the connection ends. */
static int read_exact(esp_http_client_handle_t client, char *buf, int want)
{
    int got = 0;

    while (got < want) {
        int n = esp_http_client_read(client, buf + got, want - got);
        if (n <= 0) {
            return (n < 0) ? -1 : got;
        }
        got += n;
    }

    return got;
}

/* Compares the incoming image against the one currently executing. The
   version string alone is not enough during development, where rebuilding
   without bumping PROJECT_VER is common, so the ELF digest decides when the
   strings match. */
static bool image_is_already_running(const esp_app_desc_t *incoming)
{
    const esp_app_desc_t *running = esp_app_get_description();

    if (strncmp(incoming->version, running->version,
                sizeof(incoming->version)) != 0) {
        return false;
    }

    return memcmp(incoming->app_elf_sha256, running->app_elf_sha256,
                  sizeof(incoming->app_elf_sha256)) == 0;
}

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

    char *buf = malloc(OTA_BUF_SIZE);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Out of memory");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    /* Pull enough of the body to cover the descriptor. These bytes are part
       of the image and cannot be re-read later — TCP has no rewind — so they
       stay in the buffer and become the first esp_ota_write(). */
    int head = read_exact(client, buf, OTA_HEADER_BYTES);
    if (head < (int) OTA_HEADER_BYTES) {
        ESP_LOGE(TAG, "Only %d of %u header bytes arrived",
                 head, (unsigned) OTA_HEADER_BYTES);
        free(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_app_desc_t incoming;
    memcpy(&incoming, buf + OTA_DESC_OFFSET, sizeof(incoming));

    /* A wrong magic word means the server handed us something that is not an
       ESP-IDF application: an error page, the wrong file, a truncated build. */
    if (incoming.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGE(TAG, "Not an ESP-IDF image (magic 0x%08lx)",
                 (unsigned long) incoming.magic_word);
        free(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }

    ESP_LOGI(TAG, "Server offers %s v%s (built %s %s)",
             incoming.project_name, incoming.version,
             incoming.date, incoming.time);

    if (image_is_already_running(&incoming)) {
        ESP_LOGW(TAG, "Already running this build, skipping download");
        free(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Image size %d bytes, erasing...", content_length);

    /* Passing the exact size erases only the sectors that will be used.
       OTA_SIZE_UNKNOWN would erase the full 1.9 MB and take far longer. */
    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(target, content_length, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        free(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    ESP_LOGI(TAG, "Erase done, downloading...");

    /* The bytes consumed while inspecting the header go in first. */
    int written = 0;
    err = esp_ota_write(ota_handle, buf, head);
    if (err == ESP_OK) {
        written = head;
    } else {
        ESP_LOGE(TAG, "esp_ota_write failed on the header: %s",
                 esp_err_to_name(err));
    }

    int last_logged_pct = -1;

    while (err == ESP_OK && written < content_length) {
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

    /* Only reached on failure or a skipped update: esp_restart() does not
       return. */
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "No update needed");
    } else {
        ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(err));
    }

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