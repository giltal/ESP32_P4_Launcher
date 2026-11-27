/* hid_test: the time lx_pad_wii.c stamps motion data with */
#pragma once
#include "papp/papp.h"
static inline int64_t esp_timer_get_time(void) { return papp_time_us(); }
