/*
 * Launcher status screen — shown while no app runs (until the real UI exists).
 * Also a visual self-test: corner markers check orientation, labelled bars
 * check colour order.
 */
#include "status_screen.h"
#include "bsp.h"
#include "lx_gfx.h"
#include "lx_comp.h"
#include "lx_common.h"

#include <stdio.h>
#include "esp_heap_caps.h"

#define C_BG      LX_RGB(16, 20, 32)
#define C_BAR     LX_RGB(32, 64, 128)
#define C_TEXT    LX_RGB(230, 230, 230)
#define C_DIM     LX_RGB(140, 150, 170)
#define C_OK      LX_RGB(80, 220, 120)
#define C_BAD     LX_RGB(240, 80, 80)

static char s_last_app[96];
static char s_audio[64];
static char s_input[96];
static bool s_touch_down, s_touch_seen;
static int s_touch_x, s_touch_y;

static void line(lx_canvas_t *c, int y, const char *label, const char *value, uint16_t color)
{
    lx_text(c, 24, y, label, C_DIM, 2);
    lx_text(c, 24 + 12 * 16, y, value, color, 2);
}

void status_screen_set_last_app(const char *text)
{
    snprintf(s_last_app, sizeof(s_last_app), "%s", text);
}

void status_screen_set_audio(const char *text)
{
    snprintf(s_audio, sizeof(s_audio), "%s", text);
}

void status_screen_set_input(const char *text)
{
    snprintf(s_input, sizeof(s_input), "%s", text);
}

void status_screen_set_touch(bool down, int x, int y)
{
    s_touch_down = down;
    s_touch_seen = true;
    s_touch_x = x;
    s_touch_y = y;
}

void status_screen_draw(void)
{
    uint16_t *fb = lx_comp_launcher_begin();     /* NULL while an app owns the display */
    if (!fb) return;

    const bsp_info_t *b = bsp_info();
    lx_canvas_t c;
    lx_canvas_init(&c, fb, b->display_w, b->display_h, b->display_rot);

    lx_fill(&c, 0, 0, c.w, c.h, C_BG);
    lx_fill(&c, 0, 0, c.w, 64, C_BAR);
    lx_text(&c, 24, 8, "ESP32-P4 Launcher", C_TEXT, 3);
    char buf[64];
    snprintf(buf, sizeof(buf), "v%s", LX_LAUNCHER_VERSION);
    lx_text(&c, c.w - 24 - lx_text_width(buf, 2), 18, buf, C_TEXT, 2);

    int y = 84;
    line(&c, y, "Board", b->name, C_TEXT);                                    y += 36;
    line(&c, y, "ABI", LX_ABI_STRING, C_TEXT);                                y += 36;
    line(&c, y, "SD card", bsp_sdcard_mounted() ? "mounted at /sd" : "NOT FOUND",
         bsp_sdcard_mounted() ? C_OK : C_BAD);                                y += 36;
    snprintf(buf, sizeof(buf), "%u KB internal, %u KB PSRAM",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    line(&c, y, "Free memory", buf, C_TEXT);                                  y += 36;
    line(&c, y, "DevLink", "USB-Serial-JTAG (tools/papp_dev.py)", C_OK);      y += 36;
    line(&c, y, "Last app", s_last_app[0] ? s_last_app : "-", C_TEXT);         y += 36;
    line(&c, y, "Audio", s_audio[0] ? s_audio : "not available", s_audio[0] ? C_OK : C_BAD); y += 36;
    if (!(b->caps & BSP_CAP_TOUCH))
        line(&c, y, "Touch", "not available", C_BAD);
    else if (!s_touch_seen)
        line(&c, y, "Touch", "touch the screen to test", C_DIM);
    else {
        snprintf(buf, sizeof(buf), "%s at %d, %d", s_touch_down ? "down" : "up", s_touch_x, s_touch_y);
        line(&c, y, "Touch", buf, s_touch_down ? C_OK : C_TEXT);
    }
    y += 36;
    line(&c, y, "Input", s_input[0] ? s_input : "plug in a USB gamepad or keyboard",
         s_input[0] ? C_OK : C_DIM);

    /* Colour bars: label each one so a photo / glance confirms RGB order */
    static const struct { const char *name; uint16_t color; } bars[] = {
        { "RED", LX_RGB(255, 0, 0) }, { "GREEN", LX_RGB(0, 255, 0) },
        { "BLUE", LX_RGB(0, 0, 255) }, { "WHITE", LX_RGB(255, 255, 255) },
    };
    int bw = c.w / 4, by = c.h - 72;
    for (int i = 0; i < 4; i++) {
        lx_fill(&c, i * bw, by, bw, 72, bars[i].color);
        uint16_t tc = i == 3 ? LX_RGB(0, 0, 0) : LX_RGB(255, 255, 255);
        lx_text(&c, i * bw + (bw - lx_text_width(bars[i].name, 2)) / 2, by + 20, bars[i].name, tc, 2);
    }

    /* Orientation markers in the top-left and bottom-right corners */
    lx_fill(&c, 0, 0, 12, 12, LX_RGB(255, 255, 0));
    lx_fill(&c, c.w - 12, by - 12, 12, 12, LX_RGB(255, 0, 255));

    /* Crosshair on the last touch point: confirms the touch -> landscape mapping */
    if (s_touch_seen) {
        uint16_t tc = s_touch_down ? LX_RGB(0, 255, 255) : LX_RGB(0, 120, 120);
        lx_fill(&c, s_touch_x - 20, s_touch_y - 1, 41, 3, tc);
        lx_fill(&c, s_touch_x - 1, s_touch_y - 20, 3, 41, tc);
    }

    lx_comp_launcher_end();
}
