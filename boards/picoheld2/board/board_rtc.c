/* No real-time clock on the Pico Held 2. */
#include "board_internal.h"

esp_err_t bsp_rtc_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_rtc_get(int64_t *utc)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_rtc_set(int64_t utc)
{
    return ESP_ERR_NOT_SUPPORTED;
}
