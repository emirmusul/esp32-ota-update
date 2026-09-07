#pragma once

#include "esp_ota_ops.h"

/**
 * Log the current OTA boot state: firmware version, which slot is
 * running, which slot the bootloader will pick next, and where the
 * next update will be written.
 */
void boot_info_log(void);

/**
 * Human-readable name for an OTA image state, for logs and JSON.
 */
const char *boot_info_state_str(esp_ota_img_states_t state);