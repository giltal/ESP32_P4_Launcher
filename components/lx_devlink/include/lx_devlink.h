/*
 * DevLink — remote development service over USB-Serial-JTAG.
 * Protocol: docs/ARCHITECTURE.md §7.8 and devlink_proto.h; host side: tools/papp_dev.py.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the USB-Serial-JTAG driver, hooks ESP_LOG output and starts the
 * command task. Call after the SD card is mounted and lx_app_init(). */
esp_err_t lx_devlink_start(void);

/* Log sink for app output (pass to lx_app_init). */
void lx_devlink_app_log(const char *text, size_t len);

/* App exit notification (pass to lx_app_init). */
void lx_devlink_app_exited(const char *path, int code, bool forced,
                           uint32_t leaked_allocs, uint32_t leaked_files);

#ifdef __cplusplus
}
#endif
