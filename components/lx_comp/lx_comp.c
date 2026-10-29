/*
 * Display compositor — see lx_comp.h.
 *
 * Coordinates: apps and the launcher think in landscape (W x H). The panel may
 * be mounted rotated (bsp_info()->display_rot, counter-clockwise like the PPA's
 * rotation angle), so a landscape rect is mapped to its panel rect here and the
 * PPA rotates the pixels on the way.
 */
#include "lx_comp.h"

#include <string.h>
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "lx_comp";

#define MAX_BUFFERS   2
#define FLIP_WAIT_MS  100
#define PPA_SCALE_Q   16          /* PPA scale has 4 fractional bits */

struct lx_surface {
    int w, h;
    lx_scale_t scale;
    int nbuf, cur;
    uint16_t *buf[MAX_BUFFERS];
    lx_rect_t dst;                /* landscape rect on the display */
    float sx, sy;                 /* quantised PPA scale factors   */
    uint32_t frames;
    int64_t  t_first, t_ppa;      /* stats */
};

static SemaphoreHandle_t s_lock;
static ppa_client_handle_t s_srm;
static lx_surface_t *s_surface;           /* v1: one surface at a time */
static bool s_fb_clean[2];                /* fb holds only black outside the surface rect */
static int s_dw, s_dh;                    /* landscape display size */

esp_err_t lx_comp_init(void)
{
    if (s_lock) return ESP_OK;
    if (bsp_display_fb_count() < 2) return ESP_ERR_INVALID_STATE;
    const bsp_info_t *b = bsp_info();
    bool portrait = b->display_rot == 90 || b->display_rot == 270;
    s_dw = portrait ? b->display_h : b->display_w;
    s_dh = portrait ? b->display_w : b->display_h;

    ppa_client_config_t cfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    esp_err_t err = ppa_register_client(&cfg, &s_srm);
    if (err != ESP_OK) return err;
    s_lock = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "display %dx%d (panel %ux%u, rot %u)", s_dw, s_dh, b->display_w, b->display_h, b->display_rot);
    return ESP_OK;
}

void lx_comp_display_size(int *w, int *h)
{
    *w = s_dw;
    *h = s_dh;
}

/* Landscape rect -> panel native rect, inverse of the mapping in bsp.h. */
static void to_panel(const lx_rect_t *r, int *px, int *py)
{
    switch (bsp_info()->display_rot) {
    case 270: *px = s_dh - r->y - r->h; *py = r->x; break;
    case 90:  *px = r->y;               *py = s_dw - r->x - r->w; break;
    default:  *px = r->x;               *py = r->y; break;
    }
}

static float quantise(float s)
{
    int q = (int)(s * PPA_SCALE_Q);
    if (q < 1) q = 1;
    return (float)q / PPA_SCALE_Q;
}

static void layout(lx_surface_t *s)
{
    float fx = (float)s_dw / s->w, fy = (float)s_dh / s->h;
    switch (s->scale) {
    case LX_SCALE_STRETCH:
        s->sx = quantise(fx);
        s->sy = quantise(fy);
        break;
    case LX_SCALE_INTEGER: {
        int k = (int)(fx < fy ? fx : fy);
        s->sx = s->sy = k < 1 ? quantise(fx < fy ? fx : fy) : k;
        break;
    }
    case LX_SCALE_NONE:
        s->sx = s->sy = 1.0f;
        if (s->w > s_dw || s->h > s_dh) s->sx = s->sy = quantise(fx < fy ? fx : fy);
        break;
    default:
        s->sx = s->sy = quantise(fx < fy ? fx : fy);
        break;
    }
    s->dst.w = (int)(s->sx * s->w);
    s->dst.h = (int)(s->sy * s->h);
    s->dst.x = (s_dw - s->dst.w) / 2;
    s->dst.y = (s_dh - s->dst.h) / 2;
}

lx_surface_t *lx_comp_surface_create(int w, int h, lx_scale_t scale, int buffers)
{
    if (!s_lock || w <= 0 || h <= 0 || w > 2048 || h > 2048) return NULL;
    if (buffers < 1) buffers = 1;
    if (buffers > MAX_BUFFERS) buffers = MAX_BUFFERS;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    lx_surface_t *s = NULL;
    if (s_surface) {
        ESP_LOGW(TAG, "only one surface supported for now");
        goto out;
    }
    s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_INTERNAL);
    if (!s) goto out;
    s->w = w; s->h = h; s->scale = scale; s->nbuf = buffers;
    size_t bytes = ((size_t)w * h * 2 + 63) & ~(size_t)63;
    for (int i = 0; i < buffers; i++) {
        s->buf[i] = heap_caps_aligned_calloc(64, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!s->buf[i]) {
            for (int j = 0; j < i; j++) heap_caps_free(s->buf[j]);
            heap_caps_free(s);
            s = NULL;
            goto out;
        }
    }
    layout(s);
    s_fb_clean[0] = s_fb_clean[1] = false;
    s_surface = s;
    ESP_LOGI(TAG, "surface %dx%d x%d -> %dx%d at %d,%d (scale %.4g x %.4g)", w, h, buffers,
             s->dst.w, s->dst.h, s->dst.x, s->dst.y, s->sx, s->sy);
out:
    xSemaphoreGive(s_lock);
    return s;
}

static void destroy_locked(lx_surface_t *s)
{
    if (s->frames > 1) {
        int64_t dt = esp_timer_get_time() - s->t_first;
        ESP_LOGI(TAG, "surface %dx%d: %lu frames, %.1f fps, PPA %.2f ms/frame", s->w, s->h,
                 (unsigned long)s->frames, (s->frames - 1) * 1e6 / (dt ? dt : 1),
                 s->t_ppa / 1000.0 / s->frames);
    }
    for (int i = 0; i < s->nbuf; i++) heap_caps_free(s->buf[i]);
    heap_caps_free(s);
    if (s == s_surface) s_surface = NULL;
}

void lx_comp_surface_destroy(lx_surface_t *s)
{
    if (!s || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s == s_surface) destroy_locked(s);
    xSemaphoreGive(s_lock);
}

void lx_comp_destroy_all(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_surface) destroy_locked(s_surface);
    xSemaphoreGive(s_lock);
}

uint16_t *lx_comp_lock(lx_surface_t *s, int *stride_px)
{
    if (!s || s != s_surface) return NULL;
    if (stride_px) *stride_px = s->w;
    return s->buf[s->cur];
}

lx_rect_t lx_comp_surface_rect(const lx_surface_t *s)
{
    return s->dst;
}

esp_err_t lx_comp_present(lx_surface_t *s)
{
    if (!s || !s_lock) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s != s_surface) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }

    const bsp_info_t *b = bsp_info();
    bsp_display_wait_flip(FLIP_WAIT_MS);
    int back = bsp_display_visible() ^ 1;
    uint16_t *fb = bsp_display_fb(back);
    size_t fb_bytes = (size_t)b->display_w * b->display_h * 2;
    if (!s_fb_clean[back]) {
        memset(fb, 0, fb_bytes);
        esp_cache_msync(fb, fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        s_fb_clean[back] = true;
    }

    int px, py;
    to_panel(&s->dst, &px, &py);
    ppa_srm_rotation_angle_t angle = b->display_rot == 90  ? PPA_SRM_ROTATION_ANGLE_90 :
                                     b->display_rot == 180 ? PPA_SRM_ROTATION_ANGLE_180 :
                                     b->display_rot == 270 ? PPA_SRM_ROTATION_ANGLE_270 :
                                                             PPA_SRM_ROTATION_ANGLE_0;
    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = s->buf[s->cur],
            .pic_w = s->w, .pic_h = s->h,
            .block_w = s->w, .block_h = s->h,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = fb,
            .buffer_size = fb_bytes,
            .pic_w = b->display_w, .pic_h = b->display_h,
            .block_offset_x = px, .block_offset_y = py,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = angle,
        .scale_x = s->sx,
        .scale_y = s->sy,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = ppa_do_scale_rotate_mirror(s_srm, &op);
    int64_t t1 = esp_timer_get_time();
    if (err == ESP_OK) {
        bsp_display_show(back);
        if (s->frames++ == 0) s->t_first = t1;
        s->t_ppa += t1 - t0;
        s->cur = (s->cur + 1) % s->nbuf;
    } else {
        ESP_LOGE(TAG, "PPA SRM failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    return err;
}

int lx_comp_touch_read(lx_surface_t *s, bsp_touch_point_t *pts, int max)
{
    if (!s || s != s_surface) return -1;
    bsp_touch_point_t raw[BSP_TOUCH_MAX_POINTS];
    int n = bsp_touch_read(raw, BSP_TOUCH_MAX_POINTS), out = 0;
    for (int i = 0; i < n && out < max; i++) {
        int x = raw[i].x - s->dst.x, y = raw[i].y - s->dst.y;
        if (x < 0 || y < 0 || x >= s->dst.w || y >= s->dst.h) continue;
        pts[out] = raw[i];
        pts[out].x = x * s->w / s->dst.w;
        pts[out].y = y * s->h / s->dst.h;
        out++;
    }
    return n < 0 ? n : out;
}

uint16_t *lx_comp_launcher_begin(void)
{
    if (!s_lock) return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_surface) {
        xSemaphoreGive(s_lock);
        return NULL;
    }
    bsp_display_wait_flip(FLIP_WAIT_MS);
    int back = bsp_display_visible() ^ 1;
    s_fb_clean[back] = false;
    return bsp_display_fb(back);
}

void lx_comp_launcher_end(void)
{
    const bsp_info_t *b = bsp_info();
    int back = bsp_display_visible() ^ 1;
    esp_cache_msync(bsp_display_fb(back), (size_t)b->display_w * b->display_h * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    bsp_display_show(back);
    xSemaphoreGive(s_lock);
}
