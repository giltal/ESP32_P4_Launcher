/*
 * ESP32-P4 Launcher — entry point.
 *
 * Phase 1 bring-up: board init → display, touch, audio → SD → App Manager →
 * DevLink, and a status screen while no app runs. Apps are uploaded and started remotely with
 * tools/papp_dev.py until the launcher UI exists.
 */
#include "bsp.h"
#include "lx_app.h"
#include "lx_devlink.h"
#include "lx_comp.h"
#include "lx_audio.h"
#include "lx_input.h"
#include "status_screen.h"

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

    const char *name = strrchr(path, '/');
    char text[96];
    snprintf(text, sizeof(text), "%s -> %d%s", name ? name + 1 : path, code, forced ? " (forced)" : "");
    status_screen_set_last_app(text);
    status_screen_draw();
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

    char text[64];
    snprintf(text, sizeof(text), "%d Hz, %d ms chime took %d ms", rate,
             (int)(done * 1000 / rate), ms);
    status_screen_set_audio(text);
    ESP_LOGI(TAG, "audio: %s", text);
}

/* Player 1 + devices summary for the status screen, e.g. "P1 DualShock 3: A UP" */
static void input_summary(char *buf, size_t len)
{
    static const char *const names[20] = {
        "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "SEL", "START", "L3", "R3", "HOME", "MISC",
        NULL, NULL, "UP", "DOWN", "LEFT", "RIGHT",
    };
    lx_input_describe(buf, len);
    papp_gamepad_t gp;
    size_t n = strlen(buf);
    if (lx_input_gamepad(0, &gp) && gp.buttons) {
        n += snprintf(buf + n, len - n, ":");
        for (int b = 0; b < 20 && n < len; b++)
            if ((gp.buttons & (1u << b)) && names[b]) n += snprintf(buf + n, len - n, " %s", names[b]);
    }
}

/* Status screen live lines until the launcher UI exists: touch point + input. */
static void status_task(void *arg)
{
    char input[96] = "", prev_input[96] = "";
    bool was_down = false;
    int last_x = 0, last_y = 0;
    int64_t last_draw = 0;
    for (;;) {
        bsp_touch_point_t p[BSP_TOUCH_MAX_POINTS];
        int n = (bsp_info()->caps & BSP_CAP_TOUCH) ? bsp_touch_read(p, BSP_TOUCH_MAX_POINTS) : 0;
        input_summary(input, sizeof(input));
        bool input_changed = strcmp(input, prev_input) != 0;   /* cleared when drawn */
        bool down = n > 0;
        if (down && !was_down) ESP_LOGI(TAG, "touch down %d,%d (%d points)", p[0].x, p[0].y, n);
        if (!down && was_down) ESP_LOGI(TAG, "touch up");
        if (down) status_screen_set_touch(true, p[0].x, p[0].y);
        else if (was_down) status_screen_set_touch(false, last_x, last_y);
        if (down) { last_x = p[0].x; last_y = p[0].y; }

        int64_t now = esp_timer_get_time();
        bool idle = lx_app_state() == LX_APP_IDLE;     /* a running app may have UI screens up */
        if (idle && (down != was_down || ((down || input_changed) && now - last_draw > 66000))) {   /* ~15 fps */
            strcpy(prev_input, input);
            status_screen_set_input(input);
            status_screen_draw();
            last_draw = now;
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
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
    status_screen_draw();
    xTaskCreate(status_task, "status", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "ready");
}
