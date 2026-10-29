#pragma once

#include "esp_err.h"

/* Builds the launcher home screen and starts its task (after display, input,
 * audio and the App Manager are up). */
esp_err_t home_start(void);
