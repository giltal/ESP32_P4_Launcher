/*
 * ESP32-P4 Launcher — entry point.
 *
 * Board init → display, touch, audio → SD → App Manager → input → DevLink →
 * home screen (home.c), from which apps are started; tools/papp_dev.py can
 * also upload and start them remotely.
 */
#include "bsp.h"
#include "lx_app.h"
#include "lx_devlink.h"
#include "lx_comp.h"
#include "lx_audio.h"
#include "lx_input.h"
#include "lx_net.h"
#include "home.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "launcher";

static void on_app_exit(const char *path, int code, bool forced,
                        uint32_t leaked_allocs, uint32_t leaked_files)
{
    lx_devlink_app_exited(path, code, forced, leaked_allocs, leaked_files);
}

/* Two short notes at boot. Timing the write checks the I2S clock: once the DMA
 * queue is full, writes can only proceed at the codec's sample rate. */
static void play_chime(void)
{
    const int rate = bsp_audio_rate(), note = rate / 8;     /* 125 ms per note */
    const int frames = note * 2 + rate / 10;                /* + 100 ms silence */
    int16_t *pcm = calloc(frames * 2, sizeof(int16_t));
    if (!pcm) return;
    static const float freq[2] = { 880.0f, 1318.5f };
    for (int i = 0; i < note * 2; i++) {
        float t = (float)(i % note) / rate;
        float env = 1.0f - (float)(i % note) / note;        /* plucked decay */
        int16_t v = (int16_t)(6000.0f * env * sinf(2.0f * (float)M_PI * freq[i / note] * t));
        pcm[i * 2] = pcm[i * 2 + 1] = v;
    }
    int64_t t0 = esp_timer_get_time();
    size_t done = bsp_audio_write(pcm, frames, 2000);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    free(pcm);

    ESP_LOGI(TAG, "audio: %d Hz, %d ms chime took %d ms", rate, (int)(done * 1000 / rate), ms);
}

/* HOME (or Select+Start held) — until the overlay menu exists, it quits the app. */
static void on_system_input(lx_input_sys_t ev)
{
    if (ev == LX_INPUT_SYS_HOME && lx_app_state() == LX_APP_RUNNING) {
        ESP_LOGI(TAG, "HOME: stopping %s", lx_app_path());
        lx_app_stop(2000);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(bsp_init());
    ESP_LOGI(TAG, "board: %s", bsp_info()->name);

    if (bsp_display_init() != ESP_OK || lx_comp_init() != ESP_OK)
        ESP_LOGE(TAG, "display init failed — continuing headless");

    if (bsp_touch_init() != ESP_OK)
        ESP_LOGW(TAG, "no touch");
    if (bsp_audio_init() != ESP_OK)
        ESP_LOGW(TAG, "no audio");

    if (bsp_sdcard_mount("/sd") != ESP_OK)
        ESP_LOGE(TAG, "no SD card — DevLink file commands will fail");

    ESP_ERROR_CHECK(lx_app_init(lx_devlink_app_log, on_app_exit));
    if (lx_input_init(on_system_input) != ESP_OK)
        ESP_LOGE(TAG, "input init failed — DevLink injection only");
    ESP_ERROR_CHECK(lx_devlink_start());
    if (bsp_info()->caps & BSP_CAP_AUDIO) {
        play_chime();               /* straight to the BSP, before the mixer owns it */
        if (lx_audio_init() != ESP_OK) ESP_LOGE(TAG, "audio mixer init failed");
    }
    if (lx_net_init() != ESP_OK) ESP_LOGE(TAG, "network init failed");
    if (home_start() != ESP_OK)
        ESP_LOGE(TAG, "home screen failed to start");
    ESP_LOGI(TAG, "ready");
}
