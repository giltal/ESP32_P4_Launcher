/* No touch screen on the Pico Held 2. */
#include "board_internal.h"

esp_err_t bsp_touch_init(void)
{
    return ESP_ERR_NOT_FOUND;
}

int bsp_touch_read(bsp_touch_point_t *pts, int max)
{
    return -1;
}
