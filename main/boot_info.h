#pragma once

/**
 * Log the current OTA boot state: firmware version, which slot is
 * running, which slot the bootloader will pick next, and where the
 * next update will be written.
 */
void boot_info_log(void);
