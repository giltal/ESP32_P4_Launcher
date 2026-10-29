/*
 * Minimal software drawing for launcher screens (status, crash, boot).
 * Works in logical landscape coordinates and writes straight into a panel
 * framebuffer, applying the board's rotation. Not for apps — they get the
 * compositor (Phase 2).
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t *fb;        /* panel framebuffer, native orientation */
    int pw, ph;          /* panel size                            */
    int rot;             /* 0, 90, 270 (see bsp_info_t)           */
    int w, h;            /* logical (landscape) size              */
} lx_canvas_t;

#define LX_RGB(r, g, b)  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

void lx_canvas_init(lx_canvas_t *c, uint16_t *fb, int panel_w, int panel_h, int rot);
void lx_fill(lx_canvas_t *c, int x, int y, int w, int h, uint16_t color);
/* 8x16 font, scaled by `scale`; returns the x after the last glyph. '\n' not handled. */
int  lx_text(lx_canvas_t *c, int x, int y, const char *s, uint16_t fg, int scale);
int  lx_text_width(const char *s, int scale);

#ifdef __cplusplus
}
#endif
