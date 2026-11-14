/*
 * Board buttons and battery - Pico Held 2.
 * The face buttons, D-pad, shoulder buttons and HOME are plain GPIOs. Start and Select differ between
 * the revisions (board_config.h): v1.3 has both on GPIOs (Select is active high), v1.4 reads Start
 * through the ADC. Everything is decoded into the standard gamepad layout (bsp.h BSP_BTN_*).
 * The analog stick (GPIO16/17), VEXT and the charger status are wired but have no slot in bsp.h yet.
 */
#include "board_internal.h"
#include "board_config.h"

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"

static const char *TAG = "bsp_buttons";

typedef struct {
    int gpio;
    uint32_t bit;
    bool active_high;
} dbtn_t;

static const dbtn_t s_digital[] = {
    { BOARD_BTN_A,      BSP_BTN_A,      false },
    { BOARD_BTN_B,      BSP_BTN_B,      false },
    { BOARD_BTN_X,      BSP_BTN_X,      false },
    { BOARD_BTN_Y,      BSP_BTN_Y,      false },
    { BOARD_BTN_LEFT,   BSP_BTN_LEFT,   false },
    { BOARD_BTN_RIGHT,  BSP_BTN_RIGHT,  false },
    { BOARD_BTN_UP,     BSP_BTN_UP,     false },
    { BOARD_BTN_DOWN,   BSP_BTN_DOWN,   false },
    { BOARD_BTN_L,      BSP_BTN_L1,     false },
    { BOARD_BTN_R,      BSP_BTN_R1,     false },
    { BOARD_BTN_SW,     BSP_BTN_HOME,   false },
#if PICOHELD2_REV == 13
    { BOARD_BTN_START,  BSP_BTN_START,  false },
    { BOARD_BTN_SELECT, BSP_BTN_SELECT, true  },
#else
    { BOARD_BTN_SELECT, BSP_BTN_SELECT, false },
#endif
};
#define N_DIGITAL (sizeof(s_digital) / sizeof(s_digital[0]))

static adc_oneshot_unit_handle_t s_adc[2];      /* index = adc_unit_t (ADC_UNIT_1 = 0, ADC_UNIT_2 = 1) */
static adc_unit_t s_bat_unit;
static adc_channel_t s_bat_ch;
static adc_cali_handle_t s_bat_cali;
static bool s_battery;
#if PICOHELD2_REV == 14
static adc_unit_t s_start_unit;
static adc_channel_t s_start_ch;
static bool s_start_adc;
#endif

/* Put a GPIO on its ADC channel (12 bit, 12 dB), creating the ADC unit on first use */
static bool adc_attach(int gpio, adc_unit_t *unit, adc_channel_t *ch)
{
    if (adc_oneshot_io_to_channel(gpio, unit, ch) != ESP_OK) return false;
    if (!s_adc[*unit]) {
        adc_oneshot_unit_init_cfg_t cfg = { .unit_id = *unit };
        if (adc_oneshot_new_unit(&cfg, &s_adc[*unit]) != ESP_OK) return false;
    }
    adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    return adc_oneshot_config_channel(s_adc[*unit], *ch, &ch_cfg) == ESP_OK;
}

static int adc_read(adc_unit_t unit, adc_channel_t ch)
{
    int raw = 0;
    return adc_oneshot_read(s_adc[unit], ch, &raw) == ESP_OK ? raw : 0;
}

static adc_cali_handle_t cali_create(adc_unit_t unit, adc_channel_t ch)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id = unit, .chan = ch, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    adc_cali_handle_t h = NULL;
    if (adc_cali_create_scheme_curve_fitting(&cfg, &h) == ESP_OK) return h;
#endif
    return NULL;
}

esp_err_t bsp_buttons_init(void)
{
    for (size_t i = 0; i < N_DIGITAL; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << s_digital[i].gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = s_digital[i].active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
            .pull_down_en = s_digital[i].active_high ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        };
        gpio_config(&cfg);
    }

#if PICOHELD2_REV == 14
    s_start_adc = adc_attach(BOARD_BTN_START, &s_start_unit, &s_start_ch);
    if (s_start_adc) {
        gpio_pulldown_en(BOARD_BTN_START);      /* as the vendor firmware does; the pad may not honour it in analog mode */
    } else {
        ESP_LOGW(TAG, "Start button: GPIO %d has no ADC channel", BOARD_BTN_START);
    }
#endif

    s_battery = adc_attach(BOARD_ANA_VBAT, &s_bat_unit, &s_bat_ch);
    if (s_battery) {
        s_bat_cali = cali_create(s_bat_unit, s_bat_ch);
        board_set_cap(BSP_CAP_BATTERY);
        ESP_LOGI(TAG, "battery on GPIO %d, %s", BOARD_ANA_VBAT, s_bat_cali ? "calibrated" : "uncalibrated");
    } else {
        ESP_LOGW(TAG, "battery: GPIO %d has no ADC channel", BOARD_ANA_VBAT);
    }

    board_set_cap(BSP_CAP_GPIO_PAD);
    return ESP_OK;
}

uint32_t bsp_buttons_read(void)
{
    uint32_t b = 0;
    for (size_t i = 0; i < N_DIGITAL; i++) {
        bool level = gpio_get_level(s_digital[i].gpio);
        if (level == s_digital[i].active_high) b |= s_digital[i].bit;
    }
#if PICOHELD2_REV == 14
    if (s_start_adc && adc_read(s_start_unit, s_start_ch) > BOARD_START_ADC_THRESHOLD) b |= BSP_BTN_START;
#endif
    return b;
}

int bsp_battery_mv(void)
{
    if (!s_battery) return -1;
    int sum = 0;
    for (int i = 0; i < 8; i++) sum += adc_read(s_bat_unit, s_bat_ch);
    int raw = sum / 8;

    int pin_mv;
    if (s_bat_cali) {
        if (adc_cali_raw_to_voltage(s_bat_cali, raw, &pin_mv) != ESP_OK) return -1;
    } else {
        pin_mv = raw * 3861 / 4095;     /* full scale at 12 dB, the value the 4.3" board uses without calibration */
    }
    return pin_mv * 2;                  /* 50/50 divider */
}

int bsp_analog_read(int ch)
{
    return -1;                          /* no paddle; the stick is not part of the BSP contract */
}
