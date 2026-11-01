/*
 * Launcher self-update (A/B). A launcher.bin in /sd/update is flashed to the
 * next OTA slot. A new version must call lx_update_confirm() once it works; if
 * it crashes first, the bootloader returns to the previous one.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define LX_UPDATE_FILE  "/sd/update/launcher.bin"
#define LX_UPDATE_DONE  "/sd/update/launcher.done"

/* Mark the running image good (no-op unless it is on probation). */
void lx_update_confirm(void);

/* True when the running image came back from a failed update this boot. */
bool lx_update_rolled_back(void);

/* True if LX_UPDATE_FILE is a launcher for this chip with another version. */
bool lx_update_available(char *version, size_t len);

/* Flash the file and select it for the next boot; progress in percent. The
 * caller restarts. The file is renamed first, so it can't be offered again. */
esp_err_t lx_update_install(void (*progress)(int percent));
