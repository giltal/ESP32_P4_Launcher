/*
 * DISP interface: app surfaces on top of the compositor (lx_comp).
 * Surfaces are owned by the compositor; teardown destroys whatever is left.
 */
#include "lx_services.h"
#include "lx_comp.h"

static void disp_info(papp_disp_info_t *out)
{
    SVC_ENTER();
    int w = 0, h = 0;
    lx_comp_display_size(&w, &h);
    *out = (papp_disp_info_t){ .width = w, .height = h, .refresh_hz = 60 };
    SVC_LEAVE();
}

static papp_surface_t *disp_surface_create(const papp_surface_cfg_t *cfg)
{
    if (!cfg || cfg->format != PAPP_PIXFMT_RGB565 || cfg->scale > PAPP_SCALE_NONE) return NULL;
    SVC_ENTER();
    lx_surface_t *s = lx_comp_surface_create(cfg->width, cfg->height, (lx_scale_t)cfg->scale,
                                             cfg->buffers ? cfg->buffers : 1);
    SVC_LEAVE();
    return (papp_surface_t *)s;
}

static void disp_surface_destroy(papp_surface_t *s)
{
    SVC_ENTER();
    lx_comp_surface_destroy((lx_surface_t *)s);
    SVC_LEAVE();
}

static void *disp_lock(papp_surface_t *s, int *stride_px)
{
    SVC_ENTER();
    void *p = lx_comp_lock((lx_surface_t *)s, stride_px);
    SVC_LEAVE();
    return p;
}

/* Forced stop: nothing paces the app any more (audio writes return at once), so a
 * video thread would present back to back and never be seen outside a service. */
static int disp_present(papp_surface_t *s, uint32_t flags)
{
    if (lx_svc_exit_flag) return -1;
    SVC_ENTER();
    int r = lx_comp_present((lx_surface_t *)s) == ESP_OK ? 0 : -1;
    SVC_LEAVE();
    return r;
}

static int disp_present_ext(papp_surface_t *s, const void *pixels, uint32_t flags)
{
    if (lx_svc_exit_flag) return -1;
    SVC_ENTER();
    int r = lx_comp_present_ext((lx_surface_t *)s, pixels) == ESP_OK ? 0 : -1;
    SVC_LEAVE();
    return r;
}

static int disp_touch_read(papp_surface_t *s, papp_touch_point_t *pts, int max)
{
    bsp_touch_point_t raw[BSP_TOUCH_MAX_POINTS];
    if (max > BSP_TOUCH_MAX_POINTS) max = BSP_TOUCH_MAX_POINTS;
    SVC_ENTER();
    int n = s ? lx_comp_touch_read((lx_surface_t *)s, raw, max)
              : bsp_touch_read(raw, BSP_TOUCH_MAX_POINTS);       /* display coordinates */
    SVC_LEAVE();
    if (n > max) n = max;
    for (int i = 0; i < n; i++)
        pts[i] = (papp_touch_point_t){ .x = raw[i].x, .y = raw[i].y, .size = raw[i].size, .id = raw[i].id };
    return n;
}

const papp_if_disp_t lx_if_disp = {
    .hdr = { PAPP_IF_DISP, PAPP_IF_DISP_VER, sizeof(papp_if_disp_t) },
    .info            = disp_info,
    .surface_create  = disp_surface_create,
    .surface_destroy = disp_surface_destroy,
    .lock            = disp_lock,
    .present         = disp_present,
    .touch_read      = disp_touch_read,
    .present_ext     = disp_present_ext,
};
