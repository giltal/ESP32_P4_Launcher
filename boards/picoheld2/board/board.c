/*
 * BSP implementation - Pico Held 2 (ESP32-P4, 720x720, v1.3 / v1.4).
 * Power latch, SD card and board info here; display, audio, buttons in their own files.
 */
#include "board_internal.h"
#include "board_config.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp";

static bsp_info_t s_info = {
    .id          = "picoheld2",
    .name        = BOARD_NAME,
#if BOARD_HAS_USB_HOST
    .caps        = BSP_CAP_USB_HOST,    /* the rest is set as each device comes up */
#endif
    .display_w   = BOARD_LCD_H_RES,
    .display_h   = BOARD_LCD_V_RES,
    .display_rot = PICOHELD2_LCD_ROT,
};

static bool s_sd_mounted;

/* Keep the board powered. The ON key only pulses the supply, so PWR_EN has to be driven high before the
 * key is released. A constructor runs before app_main, as early as the application code gets control.
 * The output register is set first so the pin never glitches low when it becomes an output. */
static void power_latch(void)
{
    gpio_set_level(BOARD_PWR_EN, 1);
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_PWR_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(BOARD_PWR_EN, 1);
}

static void __attribute__((constructor)) power_latch_early(void)
{
    power_latch();
}

esp_err_t bsp_init(void)
{
    power_latch();      /* no-op if the constructor already did it */

    /* Amplifier and backlight stay off until their drivers start */
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << BOARD_SND_EN) | (1ULL << BOARD_LCD_BL_EN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_set_level(BOARD_SND_EN, 0);
    gpio_set_level(BOARD_LCD_BL_EN, 0);
    gpio_config(&out);
    return ESP_OK;
}

void board_set_cap(uint32_t cap)
{
    s_info.caps |= cap;
}

const bsp_info_t *bsp_info(void)
{
    return &s_info;
}

esp_err_t bsp_sdcard_mount(const char *mount_point)
{
    if (s_sd_mounted) return ESP_OK;

    /* The card's I/O domain is fed by on-chip LDO channel 4. The host driver sets it to 3.3 V
     * when the slot starts; without that the card does not work properly. */
    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = BOARD_SD_LDO_CHAN };
    sd_pwr_ctrl_handle_t pwr = NULL;
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD LDO init failed: %s", esp_err_to_name(err));
        return err;
    }

    for (int attempt = 1; attempt <= 3; attempt++) {
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_0;
        host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
        host.pwr_ctrl_handle = pwr;

        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 4;
        slot.clk = BOARD_SD_CLK;
        slot.cmd = BOARD_SD_CMD;
        slot.d0  = BOARD_SD_D0;
        slot.d1  = BOARD_SD_D1;
        slot.d2  = BOARD_SD_D2;
        slot.d3  = BOARD_SD_D3;
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
            .format_if_mount_failed = false,
            .max_files = 16,
            .allocation_unit_size = 16 * 1024,
        };

        sdmmc_card_t *card = NULL;
        err = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot, &mount_cfg, &card);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SD mounted at %s (%s, %llu MB)", mount_point, card->cid.name,
                     ((uint64_t)card->csd.capacity * card->csd.sector_size) >> 20);
            s_sd_mounted = true;
            board_set_cap(BSP_CAP_SDCARD);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "SD mount attempt %d failed: %s", attempt, esp_err_to_name(err));

        /* Power-cycle the card through the LDO before retrying */
        sd_pwr_ctrl_del_on_chip_ldo(pwr);
        pwr = NULL;
        if (attempt == 3) break;
        vTaskDelay(pdMS_TO_TICKS(500));
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr) != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (pwr) sd_pwr_ctrl_del_on_chip_ldo(pwr);     /* free for a later try */
    return err;
}

bool bsp_sdcard_mounted(void)
{
    return s_sd_mounted;
}
