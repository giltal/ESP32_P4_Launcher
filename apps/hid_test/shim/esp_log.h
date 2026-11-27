/* hid_test: lx_pad_wii.c's logging goes to the DevLink log */
#pragma once
#include "papp/papp.h"
#define ESP_LOGI(tag, fmt, ...) papp_log("%s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) papp_log("%s: " fmt "\n", tag, ##__VA_ARGS__)
