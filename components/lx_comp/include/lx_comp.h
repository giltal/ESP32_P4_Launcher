/*
 * Display compositor (ARCHITECTURE §7.3, first cut).
 *
 * Owns the panel framebuffers. Apps draw into surfaces of any size; present()
 * scales + rotates the surface onto the hidden framebuffer with one PPA SRM
 * operation and flips at the next refresh. Launcher screens draw directly into
 * a framebuffer through lx_comp_launcher_begin/end while no surface exists.
 *
 * v1 limits: one surface at a time, RGB565 only, no overlay layer yet.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LX_SCALE_FIT_ASPECT = 0,    /* largest size that keeps the aspect ratio */
    LX_SCALE_INTEGER,           /* largest whole-number factor              */
    LX_SCALE_STRETCH,           /* fill the display                         */
    LX_SCALE_NONE,              /* 1:1, centred                             */
} lx_scale_t;

typedef struct lx_surface lx_surface_t;

typedef struct {
    int x, y, w, h;             /* landscape display coordinates */
} lx_rect_t;

esp_err_t lx_comp_init(void);

/* Display size as seen by apps (landscape, rotation already applied). */
void lx_comp_display_size(int *w, int *h);

lx_surface_t *lx_comp_surface_create(int w, int h, lx_scale_t scale, int buffers);
void          lx_comp_surface_destroy(lx_surface_t *s);
void          lx_comp_destroy_all(void);          /* app teardown */
uint16_t     *lx_comp_lock(lx_surface_t *s, int *stride_px);
esp_err_t     lx_comp_present(lx_surface_t *s);
lx_rect_t     lx_comp_surface_rect(const lx_surface_t *s);
/* Touch points mapped into surface coordinates; points outside it are dropped. */
int           lx_comp_touch_read(lx_surface_t *s, bsp_touch_point_t *pts, int max);

/* Launcher drawing: returns the hidden framebuffer (panel native layout), or
 * NULL while an app surface owns the display. end() flips it on screen. */
uint16_t *lx_comp_launcher_begin(void);
void      lx_comp_launcher_end(void);

#ifdef __cplusplus
}
#endif
