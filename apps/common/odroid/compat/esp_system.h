/* PAPP port shim */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"      /* the real header chain pulls these in */
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 256
#endif
#ifdef __cplusplus
extern "C" {
#endif
void     esp_restart(void) __attribute__((noreturn));   /* ends the app (back to the launcher) */
uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_minimum_free_heap_size(void);
#ifdef __cplusplus
}
#endif
