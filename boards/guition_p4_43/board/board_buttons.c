/*
 * Board buttons — the custom GPIO gamepad add-on and the Atari paddle.
 * Ported from RetroESP32_P4 components/odroid/odroid_input.c (Phase 40/49).
 *
 * The pad is detected at init: with the internal pull-up on the R1 pin (GPIO 29) the
 * pad's physical pull-down still reads 0. Joystick axes and A/B share ADC2
 * pins (two resistor levels each); the other buttons are active-high GPIOs.
 * A pin that reads high at init despite its pull-down is ignored (the
 * reference's phantom-R fix). Readings are decoded straight into the standard
 * gamepad layout (bsp.h BSP_BTN_*).
 */
#include "board_internal.h"
#include "board_pins.h"

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_buttons";

/* Measured on the hardware: ~3300 for the first level, ~1650 for the second */
#define ADC_HIGH     2800
#define ADC_MID_LO   1200
#define ADC_MID_HI   2200

/* Paddle pot range measured in the reference (Phase 35) */
#define PADDLE_RAW_MIN  5
#define PADDLE_RAW_MAX  3340

static const struct { int gpio; uint32_t bit; } s_digital[] = {
    { BOARD_PAD_L1,     BSP_BTN_L1 },
    { BOARD_PAD_R1,     BSP_BTN_R1 },
    { BOARD_PAD_X,      BSP_BTN_X },
    { BOARD_PAD_Y,      BSP_BTN_Y },
    { BOARD_PAD_START,  BSP_BTN_START },
    { BOARD_PAD_SELECT, BSP_BTN_SELECT },
};
#define N_DIGITAL (sizeof(s_digital) / sizeof(s_digital[0]))

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_ch_lr, s_ch_ud, s_ch_ab, s_ch_paddle, s_ch_battery;
static bool s_pad, s_paddle, s_battery;
static uint32_t s_stuck;                /* BSP_BTN_* bits whose pin reads high at rest */

static bool adc_channel(int gpio, adc_channel_t *ch)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(gpio, &unit, ch) != ESP_OK || unit != ADC_UNIT_2) return false;
    adc_oneshot_chan_cfg_t cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    return adc_oneshot_config_channel(s_adc, *ch, &cfg) == ESP_OK;
}

static int adc_read(adc_channel_t ch)
{
    int raw = 0;
    return adc_oneshot_read(s_adc, ch, &raw) == ESP_OK ? raw : 0;
}

static bool pad_detect(void)
{
    gpio_config_t det = {
        .pin_bit_mask = 1ULL << BOARD_PAD_DETECT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&det);
    vTaskDelay(pdMS_TO_TICKS(10));
    bool present = gpio_get_level(BOARD_PAD_DETECT) == 0;
    gpio_reset_pin(BOARD_PAD_DETECT);
    return present;
}

esp_err_t bsp_buttons_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_2 };
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "ADC2 init failed");
        return ESP_ERR_NOT_FOUND;
    }

    /* The paddle has no presence signal; it reads as a pot on GPIO 51 */
    s_paddle = adc_channel(BOARD_PADDLE_ADC, &s_ch_paddle);
    if (s_paddle) board_set_cap(BSP_CAP_PADDLE);
    s_battery = adc_channel(BOARD_BATTERY_ADC, &s_ch_battery);
    if (s_battery) board_set_cap(BSP_CAP_BATTERY);

    if (!pad_detect()) {
        ESP_LOGI(TAG, "GPIO gamepad not connected (GPIO %d high with pull-up)", BOARD_PAD_DETECT);
        return s_paddle ? ESP_OK : ESP_ERR_NOT_FOUND;
    }

    for (size_t i = 0; i < N_DIGITAL; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_digital[i].gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,       /* backs up the physical pull-downs */
        };
        gpio_config(&cfg);
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    for (size_t i = 0; i < N_DIGITAL; i++) {
        if (gpio_get_level(s_digital[i].gpio)) {
            s_stuck |= s_digital[i].bit;
            ESP_LOGW(TAG, "GPIO %d reads high at rest - ignored", s_digital[i].gpio);
        }
    }

    if (!adc_channel(BOARD_PAD_JOY_LR, &s_ch_lr) || !adc_channel(BOARD_PAD_JOY_UD, &s_ch_ud) ||
        !adc_channel(BOARD_PAD_AB, &s_ch_ab)) {
        ESP_LOGW(TAG, "GPIO gamepad: ADC channel setup failed");
        return s_paddle ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    s_pad = true;
    board_set_cap(BSP_CAP_GPIO_PAD);
    ESP_LOGI(TAG, "GPIO gamepad connected");
    return ESP_OK;
}

/* Two buttons on one ADC pin: level 1 (~3300) or level 2 (~1650) */
static uint32_t adc_pair(adc_channel_t ch, uint32_t high, uint32_t mid)
{
    int v = adc_read(ch);
    if (v > ADC_HIGH) return high;
    if (v > ADC_MID_LO && v < ADC_MID_HI) return mid;
    return 0;
}

uint32_t bsp_buttons_read(void)
{
    if (!s_pad) return 0;
    uint32_t b = adc_pair(s_ch_lr, BSP_BTN_LEFT, BSP_BTN_RIGHT) |
                 adc_pair(s_ch_ud, BSP_BTN_UP, BSP_BTN_DOWN) |
                 adc_pair(s_ch_ab, BSP_BTN_A, BSP_BTN_B);
    for (size_t i = 0; i < N_DIGITAL; i++)
        if (gpio_get_level(s_digital[i].gpio)) b |= s_digital[i].bit;
    return b & ~s_stuck;
}

int bsp_battery_mv(void)
{
    if (!s_battery) return -1;
    int sum = 0;
    for (int i = 0; i < 4; i++) sum += adc_read(s_ch_battery);
    /* Scale and divider (168 : 100) from the reference project */
    return sum / 4 * 3861 / 4095 * 168 / 100;
}

int bsp_analog_read(int ch)
{
    if (ch != BSP_ANALOG_PADDLE || !s_paddle) return -1;
    int raw = adc_read(s_ch_paddle);
    if (raw < PADDLE_RAW_MIN) raw = PADDLE_RAW_MIN;
    if (raw > PADDLE_RAW_MAX) raw = PADDLE_RAW_MAX;
    return (raw - PADDLE_RAW_MIN) * 65535 / (PADDLE_RAW_MAX - PADDLE_RAW_MIN);
}
