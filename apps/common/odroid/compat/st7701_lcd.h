/* PAPP port shim: ST7701 helper used for the emulators' side labels (no-op) */
#pragma once
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t st7701_lcd_draw_to_fb(int x, int y, int w, int h, const uint16_t *data);
#ifdef __cplusplus
}
#endif
