/*
 * gfx_demo — DISP interface test. Draws into a surface and lets the launcher
 * scale it onto the display.
 *   args: [width height] [fit|integer|stretch|none] [seconds] [buffers]
 *   defaults: 320 240 fit 10 2
 * The frame shows: white 1-px border on the surface edges, yellow square in the
 * top-left corner, magenta in the bottom-right, a green bar along the top, a
 * bouncing white square and a dot at each recent touch point.
 */
#include "papp/papp.h"
#include <stdlib.h>
#include <string.h>

PAPP_MANIFEST(.id = "demo.gfx", .name = "Gfx demo", .version = "0.1.0", .main_stack = 16384, .flags = PAPP_MF_HIDDEN);

#define TRAIL 64

static void fill(uint16_t *px, int stride, int w, int h, int x, int y, int rw, int rh, uint16_t c)
{
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x + rw > w) rw = w - x;
    if (y + rh > h) rh = h - y;
    for (int j = 0; j < rh; j++) {
        uint16_t *row = px + (y + j) * stride + x;
        for (int i = 0; i < rw; i++) row[i] = c;
    }
}

int main(int argc, char **argv)
{
    int w = 320, h = 240, seconds = 10, buffers = 2, scale = PAPP_SCALE_FIT_ASPECT;
    int a = 1;
    if (argc > a + 1 && atoi(argv[a]) > 0) { w = atoi(argv[a]); h = atoi(argv[a + 1]); a += 2; }
    if (argc > a && atoi(argv[a]) == 0) {
        if (!strcmp(argv[a], "integer")) scale = PAPP_SCALE_INTEGER;
        else if (!strcmp(argv[a], "stretch")) scale = PAPP_SCALE_STRETCH;
        else if (!strcmp(argv[a], "none")) scale = PAPP_SCALE_NONE;
        a++;
    }
    if (argc > a) seconds = atoi(argv[a++]);
    if (argc > a) buffers = atoi(argv[a++]);

    papp_disp_info_t info;
    if (!papp_disp_info(&info)) {
        papp_log("no display\n");
        return 2;
    }
    papp_log("display %ux%u, surface %dx%d scale %d buffers %d for %d s\n",
             info.width, info.height, w, h, scale, buffers, seconds);

    papp_surface_cfg_t cfg = { .width = w, .height = h, .format = PAPP_PIXFMT_RGB565,
                               .scale = scale, .buffers = buffers };
    papp_surface_t *s = papp_disp_surface_create(&cfg);
    if (!s) {
        papp_log("surface_create failed\n");
        return 3;
    }

    struct { int16_t x, y; } trail[TRAIL];
    int ntrail = 0, bx = 10, by = 10, dx = 3, dy = 2, box = h / 8;
    uint32_t frames = 0, touches = 0;
    int64_t t0 = papp_time_us(), t_draw = 0;
    bool was_down = false;

    while (!papp_exit_requested() && papp_time_us() - t0 < (int64_t)seconds * 1000000) {
        int stride;
        uint16_t *px = papp_disp_lock(s, &stride);
        int64_t ts = papp_time_us();

        /* Background: slow-moving diagonal colour bands */
        for (int y = 0; y < h; y++) {
            uint16_t *row = px + y * stride;
            for (int x = 0; x < w; x++) {
                int v = (x + y + (int)frames) & 0xFF;
                row[x] = PAPP_RGB565(v / 4, v / 3, 64 + v / 2);
            }
        }
        fill(px, stride, w, h, 0, 0, w, 1, 0xFFFF);
        fill(px, stride, w, h, 0, h - 1, w, 1, 0xFFFF);
        fill(px, stride, w, h, 0, 0, 1, h, 0xFFFF);
        fill(px, stride, w, h, w - 1, 0, 1, h, 0xFFFF);
        fill(px, stride, w, h, w / 4, 4, w / 2, 6, PAPP_RGB565(0, 255, 0));
        fill(px, stride, w, h, 1, 1, 16, 16, PAPP_RGB565(255, 255, 0));
        fill(px, stride, w, h, w - 17, h - 17, 16, 16, PAPP_RGB565(255, 0, 255));

        bx += dx; by += dy;
        if (bx < 0 || bx + box > w) { dx = -dx; bx += 2 * dx; }
        if (by < 0 || by + box > h) { dy = -dy; by += 2 * dy; }
        fill(px, stride, w, h, bx, by, box, box, 0xFFFF);

        papp_touch_point_t tp[5];
        int n = papp_disp_touch(s, tp, 5);
        if (n > 0) {
            if (!was_down) {
                touches++;
                papp_log("touch %u,%u\n", tp[0].x, tp[0].y);
            }
            trail[ntrail % TRAIL].x = tp[0].x;
            trail[ntrail % TRAIL].y = tp[0].y;
            ntrail++;
        }
        was_down = n > 0;
        for (int i = 0; i < ntrail && i < TRAIL; i++)
            fill(px, stride, w, h, trail[i].x - 2, trail[i].y - 2, 5, 5, PAPP_RGB565(255, 64, 0));

        t_draw += papp_time_us() - ts;
        papp_disp_present(s);
        frames++;
    }

    int64_t dt = papp_time_us() - t0;
    papp_log("%lu frames in %lld ms = %ld.%01ld fps, draw %ld us/frame, %lu touches\n",
             (unsigned long)frames, dt / 1000, (long)(frames * 1000000LL / dt),
             (long)(frames * 10000000LL / dt % 10), (long)(t_draw / (frames ? frames : 1)),
             (unsigned long)touches);
    papp_disp_surface_destroy(s);
    return 0;
}
