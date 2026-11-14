/*
 * BSP display - HD395003C30 720x720 MIPI-DSI panel (2 lanes, 600 Mbps), double-buffered DPI, RGB565.
 * Init table, timing and reset behaviour come from lcd_hd395003c30.h. The panel is mounted rotated
 * 90 deg clockwise (see PICOHELD2_LCD_ROT in board_config.h). The panel is driven through the stock
 * esp_lcd DPI panel; the vendor init commands and the reset are done here.
 */
#include "board_internal.h"
#include "board_config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bsp_disp";

#define NUM_FBS 2

/* Pixel clock and blanking, from the vendor driver (about 53 Hz refresh) */
#define PANEL_PCLK_MHZ  41
#define PANEL_HPW       60
#define PANEL_HBP       120
#define PANEL_HFP       106
#define PANEL_VPW       4
#define PANEL_VBP       20
#define PANEL_VFP       20

typedef struct {
    uint8_t cmd;
    const uint8_t *data;
    uint8_t len;
    uint16_t delay_ms;
} init_cmd_t;

#define CMD(c, ...) { (c), (uint8_t[]){ __VA_ARGS__ }, sizeof((uint8_t[]){ __VA_ARGS__ }), 0 }
#define CMD_DELAY(c, ms) { (c), (uint8_t[]){ 0x00 }, 0, (ms) }

static const init_cmd_t s_panel_init[] = {
    CMD(0xB9,
        0xF1, 0x12, 0x87),
    CMD(0xB2,
        0xB4, 0x03, 0x70),
    CMD(0xB3,
        0x10, 0x10, 0x28, 0x28, 0x03, 0xFF, 0x00, 0x00, 0x00, 0x00),
    CMD(0xB4,
        0x80),
    CMD(0xB5,
        0x0A, 0x0A),
    CMD(0xB6,
        0x8D, 0x8D),
    CMD(0xB8,
        0x26, 0x22, 0xF0, 0x13),
    CMD(0xBA,
        0x31, 0x81, 0x05, 0xF9, 0x0E, 0x0E, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x44, 0x25, 0x00, 0x91, 0x0A, 0x00, 0x00, 0x01, 0x4F, 0x01,
        0x00, 0x00, 0x37),
    CMD(0xBC,
        0x47),
    CMD(0xBF,
        0x02, 0x10, 0x00, 0x80, 0x04),
    CMD(0xC0,
        0x73, 0x73, 0x50, 0x50, 0x00, 0x00, 0x12, 0x73, 0x00),
    CMD(0xC1,
        0x36, 0x00, 0x32, 0x32, 0x77, 0xE1, 0x77, 0x77, 0xCC, 0xCC, 0xFF, 0xFF,
        0x11, 0x11, 0x00, 0x00, 0x32),
    CMD(0xC7,
        0x10, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0xED, 0xC5, 0x00, 0xA5),
    CMD(0xC8,
        0x10, 0x40, 0x1E, 0x03),
    CMD(0xCC,
        0x0B),
    CMD(0xE0,
        0x00, 0x0A, 0x0F, 0x2A, 0x33, 0x3F, 0x44, 0x39, 0x06, 0x0C, 0x0E, 0x14,
        0x15, 0x13, 0x15, 0x10, 0x18, 0x00, 0x0A, 0x0F, 0x2A, 0x33, 0x3F, 0x44,
        0x39, 0x06, 0x0C, 0x0E, 0x14, 0x15, 0x13, 0x15, 0x10, 0x18),
    CMD(0xE1,
        0x11, 0x11, 0x91, 0x00, 0x00, 0x00, 0x00),
    CMD(0xE3,
        0x07, 0x07, 0x0B, 0x0B, 0x0B, 0x0B, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x04,
        0xC0, 0x10),
    CMD(0xE9,
        0xC8, 0x10, 0x0A, 0x00, 0x00, 0x80, 0x81, 0x12, 0x31, 0x23, 0x4F, 0x86,
        0xA0, 0x00, 0x47, 0x08, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0C, 0x00, 0x00, 0x00, 0x98, 0x02, 0x8B, 0xAF, 0x46, 0x02, 0x88, 0x88,
        0x88, 0x88, 0x88, 0x98, 0x13, 0x8B, 0xAF, 0x57, 0x13, 0x88, 0x88, 0x88,
        0x88, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00),
    CMD(0xEA,
        0x97, 0x0C, 0x09, 0x09, 0x09, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x9F, 0x31, 0x8B, 0xA8, 0x31, 0x75, 0x88, 0x88, 0x88, 0x88, 0x88, 0x9F,
        0x20, 0x8B, 0xA8, 0x20, 0x64, 0x88, 0x88, 0x88, 0x88, 0x88, 0x23, 0x00,
        0x00, 0x02, 0x71, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x80, 0x81, 0x00, 0x00, 0x00,
        0x00),
    CMD(0xEF,
        0xFF, 0xFF, 0x01),
    CMD_DELAY(0x11, 250),
    CMD_DELAY(0x29, 50),
};

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fbs[NUM_FBS];
static int s_visible;
static volatile bool s_flip_pending;
static SemaphoreHandle_t s_vsync;

static bool IRAM_ATTR on_refresh_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata,
                                      void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync, &woken);
    return woken == pdTRUE;
}

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BOARD_LCD_BL_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num = BOARD_LCD_BL_PWM,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ledc_channel_config(&c);

    gpio_set_level(BOARD_LCD_BL_EN, 0);
    gpio_config_t en = { .pin_bit_mask = 1ULL << BOARD_LCD_BL_EN, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&en);
}

/* Hardware reset. IOVCC is 1.8 V and the reset line has an external pull-up, so the pin is
 * released (input, no pull) instead of being driven high. */
static void panel_hw_reset(void)
{
    gpio_reset_pin(BOARD_LCD_RST);
    gpio_set_direction(BOARD_LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_direction(BOARD_LCD_RST, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_LCD_RST, GPIO_FLOATING);
    vTaskDelay(pdMS_TO_TICKS(150));
}

static esp_err_t panel_send_init(esp_lcd_panel_io_handle_t io)
{
    for (size_t i = 0; i < sizeof(s_panel_init) / sizeof(s_panel_init[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, s_panel_init[i].cmd, s_panel_init[i].data,
                                                      s_panel_init[i].len), TAG, "init cmd 0x%02X", s_panel_init[i].cmd);
        if (s_panel_init[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(s_panel_init[i].delay_ms));
    }
    return ESP_OK;
}

esp_err_t bsp_display_init(void)
{
    if (s_panel) return ESP_OK;
    backlight_init();

    /* MIPI D-PHY power */
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = BOARD_DSI_PHY_LDO_CHAN, .voltage_mv = BOARD_DSI_PHY_LDO_MV };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "DSI PHY LDO");

    esp_lcd_dsi_bus_handle_t bus;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 600,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "DSI bus");

    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &dbi_cfg, &io), TAG, "DBI io");

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = PANEL_PCLK_MHZ,
        .virtual_channel = 0,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = NUM_FBS,
        .video_timing = {
            .h_size = BOARD_LCD_H_RES,
            .v_size = BOARD_LCD_V_RES,
            .hsync_pulse_width = PANEL_HPW,
            .hsync_back_porch = PANEL_HBP,
            .hsync_front_porch = PANEL_HFP,
            .vsync_pulse_width = PANEL_VPW,
            .vsync_back_porch = PANEL_VBP,
            .vsync_front_porch = PANEL_VFP,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(bus, &dpi_cfg, &s_panel), TAG, "DPI panel");

    /* Same order as the vendor driver: reset, init commands (they end with sleep-out and display-on),
     * then start the DPI video stream. */
    panel_hw_reset();
    ESP_RETURN_ON_ERROR(panel_send_init(io), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "DPI init");

    void *fb0 = NULL, *fb1 = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, NUM_FBS, &fb0, &fb1), TAG, "fbs");
    s_fbs[0] = fb0;
    s_fbs[1] = fb1;

    s_vsync = xSemaphoreCreateBinary();
    esp_lcd_dpi_panel_event_callbacks_t cbs = { .on_refresh_done = on_refresh_done };
    esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL);

    /* Black in both buffers before the backlight comes on (no white flash) */
    size_t bytes = BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2;
    for (int i = 0; i < NUM_FBS; i++) {
        memset(s_fbs[i], 0, bytes);
        esp_cache_msync(s_fbs[i], bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    bsp_display_show(0);
    bsp_display_backlight(100);
    board_set_cap(BSP_CAP_DISPLAY);

    ESP_LOGI(TAG, "HD395003C30 %dx%d up, fb0=%p fb1=%p", BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_fbs[0], s_fbs[1]);
    return ESP_OK;
}

int bsp_display_fb_count(void)
{
    return s_panel ? NUM_FBS : 0;
}

uint16_t *bsp_display_fb(int index)
{
    return (s_panel && index >= 0 && index < NUM_FBS) ? s_fbs[index] : NULL;
}

esp_err_t bsp_display_show(int index)
{
    if (!s_panel || index < 0 || index >= NUM_FBS) return ESP_ERR_INVALID_ARG;
    /* Passing one of the driver's own framebuffers makes it switch scan-out to
     * that buffer (after a cache write-back) instead of copying. */
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_fbs[index]);
    if (err == ESP_OK) {
        s_visible = index;
        xSemaphoreTake(s_vsync, 0);
        s_flip_pending = true;
    }
    return err;
}

bool bsp_display_wait_flip(uint32_t timeout_ms)
{
    if (!s_flip_pending) return true;
    bool ok = xSemaphoreTake(s_vsync, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (ok) s_flip_pending = false;
    return ok;
}

int bsp_display_visible(void)
{
    return s_visible;
}

bool bsp_display_wait_vsync(uint32_t timeout_ms)
{
    if (!s_vsync) return false;
    xSemaphoreTake(s_vsync, 0);     /* drop a stale edge */
    return xSemaphoreTake(s_vsync, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void bsp_display_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    gpio_set_level(BOARD_LCD_BL_EN, percent > 0);       /* driver off at 0 */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023 * percent / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
