/*
 * Clock — big clock, stopwatch and countdown timer. Time and time zone come from
 * the launcher (NTP or DevLink).
 *   X / START  mode menu
 *   Stopwatch  A start/stop, B reset
 *   Timer      UP/DOWN minutes, A start/stop, B reset
 */
#include <math.h>
#include <string.h>
#include "papp/papp.h"

PAPP_MANIFEST(.id = "tool.clock", .name = "Clock", .version = "1.0.0", .main_stack = 16384);

#define W 400
#define H 240

enum { MODE_CLOCK, MODE_STOPWATCH, MODE_TIMER };

static uint16_t *s_px;
static int s_stride;

static void rect(int x, int y, int w, int h, uint16_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) s_px[j * s_stride + i] = c;
}

/* Seven-segment digit in a box of w x h */
static void digit(int x, int y, int w, int h, int t, int d, uint16_t c)
{
    static const uint8_t seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    uint8_t s = d >= 0 && d <= 9 ? seg[d] : 0x40;
    int mid = y + (h - t) / 2;
    if (s & 0x01) rect(x + t, y, w - 2 * t, t, c);                     /* top */
    if (s & 0x02) rect(x + w - t, y + t, t, (h - 3 * t) / 2, c);       /* upper right */
    if (s & 0x04) rect(x + w - t, mid + t, t, (h - 3 * t) / 2, c);     /* lower right */
    if (s & 0x08) rect(x + t, y + h - t, w - 2 * t, t, c);             /* bottom */
    if (s & 0x10) rect(x, mid + t, t, (h - 3 * t) / 2, c);             /* lower left */
    if (s & 0x20) rect(x, y + t, t, (h - 3 * t) / 2, c);               /* upper left */
    if (s & 0x40) rect(x + t, mid, w - 2 * t, t, c);                   /* middle */
}

/* "12:34" style: pairs of digits separated by colons, centred at y */
static void digits(const int *v, int pairs, int y, int w, int h, int t, uint16_t c)
{
    int gap = t * 2, colon = t * 3;
    int total = pairs * (2 * w + t) + (pairs - 1) * colon;
    int x = (W - total) / 2;
    for (int p = 0; p < pairs; p++) {
        digit(x, y, w, h, t, v[p] / 10, c);
        digit(x + w + t, y, w, h, t, v[p] % 10, c);
        x += 2 * w + t;
        if (p + 1 < pairs) {
            rect(x + t, y + h / 3, t, t, c);
            rect(x + t, y + 2 * h / 3 - t, t, t, c);
            x += colon;
        }
    }
    (void)gap;
}

static void beep(papp_audio_stream_t *a)
{
    if (!a) return;
    static int16_t pcm[1600];
    for (int i = 0; i < 1600; i++)
        pcm[i] = (int16_t)(9000.0f * sinf((float)i * 2.0f * 3.14159f * 880.0f / 16000.0f));
    papp_audio_write(a, pcm, 1600, 0);
}

int main(int argc, char **argv)
{
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *s = papp_disp_surface_create(&cfg);
    if (!s) return 1;
    papp_audio_cfg_t ac = { .rate = 16000, .channels = 1, .format = PAPP_AUDIO_S16 };
    papp_audio_stream_t *audio = papp_audio_open(&ac);

    const uint16_t bg = PAPP_RGB565(6, 12, 28), fg = PAPP_RGB565(120, 200, 255);
    const uint16_t dim = PAPP_RGB565(70, 90, 130), on = PAPP_RGB565(120, 255, 170);
    int mode = MODE_CLOCK, timer_min = 5;
    int64_t sw_base = 0, sw_start = 0, tm_left = 5 * 60 * 1000000LL, tm_last = 0;
    bool sw_run = false, tm_run = false;
    uint32_t prev = 0;

    while (!papp_exit_requested()) {
        papp_gamepad_t pad = { 0 };
        papp_input_gamepad(0, &pad);
        uint32_t press = pad.buttons & ~prev;
        prev = pad.buttons;
        int64_t now = papp_time_us();

        if (press & (PAPP_BTN_X | PAPP_BTN_START)) {
            static const char *const items[] = { "Clock", "Stopwatch", "Timer", "Exit" };
            int r = papp_ui_menu("Clock", items, 4, mode);
            if (r == 3) break;
            if (r >= 0) mode = r;
            prev = 0xFFFFFFFFu;             /* ignore what closed the menu until released */
            continue;
        }
        if (mode == MODE_STOPWATCH) {
            if (press & PAPP_BTN_A) {
                if (sw_run) sw_base += now - sw_start;
                else sw_start = now;
                sw_run = !sw_run;
            }
            if (press & PAPP_BTN_B) { sw_run = false; sw_base = 0; }
        } else if (mode == MODE_TIMER) {
            if (!tm_run) {
                if (press & PAPP_BTN_UP) timer_min = timer_min >= 99 ? 99 : timer_min + 1;
                if (press & PAPP_BTN_DOWN) timer_min = timer_min <= 1 ? 1 : timer_min - 1;
                if (press & (PAPP_BTN_UP | PAPP_BTN_DOWN)) tm_left = timer_min * 60 * 1000000LL;
            }
            if (press & PAPP_BTN_A) {
                if (tm_left <= 0) tm_left = timer_min * 60 * 1000000LL;
                tm_run = !tm_run;
                tm_last = now;
            }
            if (press & PAPP_BTN_B) { tm_run = false; tm_left = timer_min * 60 * 1000000LL; }
        }
        if (tm_run) {
            tm_left -= now - tm_last;
            tm_last = now;
            if (tm_left <= 0) {
                tm_left = 0;
                tm_run = false;
                for (int i = 0; i < 3; i++) beep(audio);
            }
        }

        s_px = papp_disp_lock(s, &s_stride);
        rect(0, 0, W, H, bg);
        if (mode == MODE_CLOCK) {
            papp_datetime_t t;
            if (papp_local_time(&t)) {
                int hm[3] = { t.hour, t.minute, t.second };
                digits(hm, 3, 60, 44, 90, 7, fg);
                int date[3] = { t.day, t.month, t.year % 100 };
                digits(date, 3, 176, 18, 36, 3, dim);
            } else {
                int none[1] = { -1 };
                (void)none;
                for (int i = 0; i < 4; i++) rect(100 + i * 52, 110, 40, 8, dim);
            }
        } else if (mode == MODE_STOPWATCH) {
            int64_t us = sw_base + (sw_run ? now - sw_start : 0);
            int cs = (int)(us / 10000 % 100), sec = (int)(us / 1000000);
            int v[3] = { sec / 60 % 100, sec % 60, cs };
            digits(v, 3, 75, 44, 90, 7, sw_run ? on : fg);
        } else {
            int sec = (int)((tm_left + 999999) / 1000000);
            int v[2] = { sec / 60, sec % 60 };
            digits(v, 2, 70, 60, 100, 9, tm_run ? on : tm_left == 0 ? PAPP_RGB565(255, 90, 90) : fg);
        }
        papp_disp_present(s);
    }
    if (audio) papp_audio_close(audio);
    papp_disp_surface_destroy(s);
    return 0;
}
