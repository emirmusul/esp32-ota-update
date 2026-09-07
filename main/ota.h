#pragma once

#include "esp_err.h"

/**
 * Download a firmware image over HTTP and write it into the inactive
 * OTA slot. Does not change the boot partition.
 *
 * @param url  HTTP URL of the .bin image.
 * @return ESP_OK if the image was written and verified.
 */
esp_err_t ota_download_to_inactive_slot(const char *url);

/**
 * Download an image, mark the slot it was written to as the boot
 * partition, and reboot into it. Does not return on success.
 *
 * @param url  HTTP URL of the .bin image, or NULL to use CONFIG_OTA_UPDATE_URL.
 * @return An error code if any step failed. Never returns on success.
 */
esp_err_t ota_update_and_reboot(const char *url);

/**
 * Run ota_update_and_reboot() in a background task so the caller
 * (typically an HTTP handler) can return immediately.
 *
 * @return ESP_OK if the task was created, ESP_ERR_INVALID_STATE if an
 *         update is already running.
 */
esp_err_t ota_trigger_async(void);