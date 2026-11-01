/*
 * Launcher home screen (design "Option 2", landscape): header with logo, clock,
 * SD card and settings; the installed apps, the last one run first; a system bar.
 * Plus a settings screen (volume, brightness, system info).
 *
 * LVGL 9 renders the whole 800x480 screen into one PSRAM buffer (direct mode);
 * after each update the compositor rotates it onto the panel. All LVGL calls
 * happen on the home task. While an app runs the task only waits: the app
 * owns the display and input, and the home screen is redrawn when it exits.
 *
 * Apps: /sd/apps/<dir>/<file>.papp, named by their manifest, with an optional
 * /sd/apps/<dir>/icon.png (any size; shown at 88x88). Gamepad / keyboard move
 * the focus to the nearest item in the direction pressed; A / Enter starts.
 */
#include "home.h"
#include "bsp.h"
#include "lx_app.h"
#include "lx_audio.h"
#include "lx_comp.h"
#include "lx_common.h"
#include "lx_input.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/temperature_sensor.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "home";

/* Paths are capped on purpose; a longer one is cut, not overflowed */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define APPS_DIR      "/sd/apps"
#define LAST_APP_FILE "/sd/launcher/last_app.txt"
#define MAX_APPS      48
#define ICON          88

/* Palette (from the design mock-up) */
#define C_BG_TOP      lv_color_hex(0x0B1630)
#define C_BG_BOTTOM   lv_color_hex(0x050A14)
#define C_PANEL       lv_color_hex(0x0E1A30)
#define C_BORDER      lv_color_hex(0x24406E)
#define C_ACCENT      lv_color_hex(0x3BC9F5)
#define C_BLUE        lv_color_hex(0x1E6FFF)
#define C_TEXT        lv_color_hex(0xFFFFFF)
#define C_DIM         lv_color_hex(0x8FA3BF)
#define C_GREEN       lv_color_hex(0x3DDC84)
#define C_FOCUS       lv_color_hex(0x1B3A70)

static const uint32_t TILE_COLORS[] = { 0x2F7BF6, 0x8E44EC, 0x22B35E, 0xF39C12, 0x12A7A7, 0xE8457C, 0x5B4BDB, 0x3B4A63 };

typedef struct {
    char path[160];             /* .papp */
    char icon[176];             /* "S:/sd/apps/x/icon.png" or "" */
    char name[32];
    char id[32];
    bool hidden;                /* PAPP_MF_HIDDEN */
} app_entry_t;

static app_entry_t s_apps[MAX_APPS];
static int s_napps;
static char s_last[160];        /* last launched .papp */

static lv_display_t *s_disp;
static uint16_t *s_frame;
static lv_obj_t *s_home, *s_settings;
static lv_obj_t *s_clock, *s_date, *s_sd_free, *s_sd_apps, *s_grid, *s_count;
static lv_obj_t *s_sys_cpu, *s_sys_sram, *s_sys_psram, *s_sys_temp, *s_sys_pads, *s_sys_state;
static lv_obj_t *s_vol_slider, *s_bri_slider, *s_vol_val, *s_bri_val, *s_info;
static temperature_sensor_handle_t s_tsens;
static char s_launch[160];      /* set by a click; launched by the task loop */
static int s_volume = 60, s_brightness = 100;
static bool s_show_dev;          /* show PAPP_MF_HIDDEN apps */
static lv_obj_t *s_dev_switch;

/* ── Focus (gamepad / keyboard): nearest item in the pressed direction ──── */

#define MAX_FOCUS (MAX_APPS + 8)
static lv_obj_t *s_focusable[MAX_FOCUS];
static int s_nfocus;
static lv_obj_t *s_focused;

static void focus_clear_list(void)
{
    s_nfocus = 0;
    s_focused = NULL;
}

static void focus_add(lv_obj_t *o)
{
    if (s_nfocus < MAX_FOCUS) s_focusable[s_nfocus++] = o;
}

static void focus_set(lv_obj_t *o)
{
    if (s_focused == o) return;
    if (s_focused) lv_obj_remove_state(s_focused, LV_STATE_FOCUSED);
    s_focused = o;
    if (o) {
        lv_obj_add_state(o, LV_STATE_FOCUSED);
        lv_obj_scroll_to_view_recursive(o, LV_ANIM_ON);
    }
}

static void focus_move(int dx, int dy)
{
    if (!s_focused) {
        if (s_nfocus) focus_set(s_focusable[0]);
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(s_focused, &a);
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    lv_obj_t *best = NULL;
    int best_score = 1 << 30;
    for (int i = 0; i < s_nfocus; i++) {
        lv_obj_t *o = s_focusable[i];
        if (o == s_focused) continue;
        lv_area_t b;
        lv_obj_get_coords(o, &b);
        int ox = (b.x1 + b.x2) / 2 - cx, oy = (b.y1 + b.y2) / 2 - cy;
        int along = ox * dx + oy * dy, across = abs(dx ? oy : ox);
        if (along <= 0) continue;
        int score = along + 3 * across;
        if (score < best_score) { best_score = score; best = o; }
    }
    if (best) focus_set(best);
}

/* ── Apps ──────────────────────────────────────────────────────────────── */

static int app_cmp(const void *a, const void *b)
{
    return strcasecmp(((const app_entry_t *)a)->name, ((const app_entry_t *)b)->name);
}

static void scan_apps(void)
{
    s_napps = 0;
    DIR *d = opendir(APPS_DIR);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && s_napps < MAX_APPS) {
        if (de->d_name[0] == '.' || de->d_type != DT_DIR) continue;
        char dir[96];
        snprintf(dir, sizeof(dir), APPS_DIR "/%s", de->d_name);
        DIR *sd = opendir(dir);
        if (!sd) continue;
        struct dirent *fe;
        while ((fe = readdir(sd))) {
            size_t n = strlen(fe->d_name);
            if (n < 6 || strcasecmp(fe->d_name + n - 5, ".papp") != 0) continue;
            app_entry_t *e = &s_apps[s_napps];
            memset(e, 0, sizeof(*e));
            snprintf(e->path, sizeof(e->path), "%s/%s", dir, fe->d_name);
            papp_manifest_t m;
            if (lx_app_read_manifest(e->path, &m) == ESP_OK) {
                snprintf(e->name, sizeof(e->name), "%s", m.name);
                snprintf(e->id, sizeof(e->id), "%s", m.id);
                e->hidden = m.flags & PAPP_MF_HIDDEN;
            } else {
                snprintf(e->name, sizeof(e->name), "%.*s", (int)(n - 5), fe->d_name);
            }
            char icon[128];
            struct stat st;
            snprintf(icon, sizeof(icon), "%s/icon.png", dir);
            if (stat(icon, &st) == 0) snprintf(e->icon, sizeof(e->icon), "S:%s", icon);
            if (!e->hidden || s_show_dev) s_napps++;
            break;                                  /* one app per folder */
        }
        closedir(sd);
    }
    closedir(d);
    qsort(s_apps, s_napps, sizeof(app_entry_t), app_cmp);

    s_last[0] = 0;
    FILE *f = fopen(LAST_APP_FILE, "r");
    if (f) {
        if (fgets(s_last, sizeof(s_last), f)) s_last[strcspn(s_last, "\r\n")] = 0;
        fclose(f);
    }
    /* The last app run comes first (and gets the focus) */
    for (int i = 1; i < s_napps; i++) {
        if (strcmp(s_apps[i].path, s_last) != 0) continue;
        app_entry_t last = s_apps[i];
        memmove(&s_apps[1], &s_apps[0], i * sizeof(app_entry_t));
        s_apps[0] = last;
        break;
    }
}

static void remember_last(const char *path)
{
    mkdir("/sd/launcher", 0775);
    FILE *f = fopen(LAST_APP_FILE, "w");
    if (f) {
        fprintf(f, "%s\n", path);
        fclose(f);
    }
    snprintf(s_last, sizeof(s_last), "%s", path);
}

/* An app icon: its PNG scaled to size, or a coloured tile with its initial. */
static lv_obj_t *make_icon(lv_obj_t *parent, const app_entry_t *e, int size)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, size, size);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(box, size / 4, 0);
    lv_obj_set_style_clip_corner(box, true, 0);

    if (e && e->icon[0]) {
        lv_image_header_t h;
        if (lv_image_decoder_get_info(e->icon, &h) == LV_RESULT_OK && h.w && h.h) {
            lv_obj_t *img = lv_image_create(box);
            lv_image_set_src(img, e->icon);
            int big = h.w > h.h ? h.w : h.h;
            lv_image_set_scale(img, (uint32_t)(256 * size / big));
            lv_obj_center(img);
            return box;
        }
    }
    uint32_t hash = 5381;
    for (const char *p = e ? (e->id[0] ? e->id : e->name) : "?"; *p; p++) hash = hash * 33 + (uint8_t)*p;
    uint32_t c = TILE_COLORS[hash % (sizeof(TILE_COLORS) / sizeof(TILE_COLORS[0]))];
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_lighten(lv_color_hex(c), 40), 0);
    lv_obj_set_style_bg_grad_color(box, lv_color_hex(c), 0);
    lv_obj_set_style_bg_grad_dir(box, LV_GRAD_DIR_VER, 0);
    lv_obj_t *l = lv_label_create(box);
    char initial[2] = { e && e->name[0] ? (char)toupper((unsigned char)e->name[0]) : '?', 0 };
    lv_label_set_text(l, initial);
    lv_obj_set_style_text_font(l, size >= 80 ? &lv_font_montserrat_48 : &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, C_TEXT, 0);
    lv_obj_center(l);
    return box;
}

/* ── Clicks ────────────────────────────────────────────────────────────── */

static void on_tile_click(lv_event_t *ev)
{
    const app_entry_t *e = lv_event_get_user_data(ev);
    snprintf(s_launch, sizeof(s_launch), "%s", e->path);
}

static void show_settings(void);
static void show_home(void);

static void on_gear_click(lv_event_t *ev)   { show_settings(); }
static void on_back_click(lv_event_t *ev)   { show_home(); }

/* ── Home screen ───────────────────────────────────────────────────────── */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, int radius)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(p, radius, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(p, C_PANEL, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, C_BORDER, 0);
    return p;
}

/* Focus look for clickable things: a bright outline */
static void focus_style(lv_obj_t *o, int radius)
{
    lv_obj_set_style_outline_width(o, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(o, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(o, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(o, radius, 0);
}

static void build_grid(void)
{
    lv_obj_clean(s_grid);
    for (int i = 0; i < s_napps; i++) {
        app_entry_t *e = &s_apps[i];
        lv_obj_t *t = lv_obj_create(s_grid);
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, 118, 128);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        focus_style(t, 18);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(t, C_FOCUS, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(t, C_FOCUS, LV_STATE_PRESSED);
        lv_obj_t *ic = make_icon(t, e, ICON);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_t *l = label(t, &lv_font_montserrat_16, C_TEXT, e->name);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, 112);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -6);
        lv_obj_add_event_cb(t, on_tile_click, LV_EVENT_CLICKED, e);
    }
    if (!s_napps) {
        lv_obj_t *l = label(s_grid, &lv_font_montserrat_16, C_DIM,
                            "No apps on the SD card yet (put them in /sd/apps/<name>/<name>.papp)");
        lv_obj_set_style_pad_top(l, 40, 0);
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%d installed " LV_SYMBOL_RIGHT, s_napps);
    lv_label_set_text(s_count, buf);
}

static lv_obj_t *s_gear;

static void home_focus_list(void)
{
    lv_obj_t *keep = s_focused;
    focus_clear_list();
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_grid); i++) {
        lv_obj_t *c = lv_obj_get_child(s_grid, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE)) focus_add(c);
    }
    focus_add(s_gear);
    bool still_there = false;
    for (int i = 0; i < s_nfocus; i++) still_there |= s_focusable[i] == keep;
    s_focused = NULL;
    focus_set(still_there ? keep : (s_nfocus ? s_focusable[0] : NULL));
    if (!still_there) lv_obj_scroll_to_y(s_grid, 0, LV_ANIM_OFF);
}

static void build_home(void)
{
    s_home = lv_obj_create(NULL);
    lv_obj_remove_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_home, C_BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(s_home, C_BG_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(s_home, LV_GRAD_DIR_VER, 0);

    /* Header: logo, clock, SD card, settings */
    lv_obj_t *p4 = label(s_home, &lv_font_montserrat_48, C_ACCENT, "P4");
    lv_obj_set_pos(p4, 18, 6);
    lv_obj_t *name = label(s_home, &lv_font_montserrat_48, C_TEXT, "Launcher");
    lv_obj_set_pos(name, 92, 6);
    lv_obj_t *sub = label(s_home, &lv_font_montserrat_16, C_DIM, "ESP32-P4  Application Hub");
    lv_obj_set_pos(sub, 22, 62);

    s_clock = label(s_home, &lv_font_montserrat_48, C_TEXT, "--:--");
    lv_obj_set_pos(s_clock, 352, 6);
    s_date = label(s_home, &lv_font_montserrat_16, C_DIM, "");
    lv_obj_set_pos(s_date, 356, 62);

    lv_obj_t *sd = panel(s_home, 506, 10, 214, 76, 14);
    lv_obj_t *sdi = label(sd, &lv_font_montserrat_28, C_GREEN, LV_SYMBOL_SD_CARD);
    lv_obj_set_pos(sdi, 14, 20);
    lv_obj_t *sdt = label(sd, &lv_font_montserrat_16, C_TEXT, "SD Card");
    lv_obj_set_pos(sdt, 52, 6);
    s_sd_free = label(sd, &lv_font_montserrat_16, C_GREEN, "");
    lv_obj_set_pos(s_sd_free, 52, 28);
    s_sd_apps = label(sd, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_set_pos(s_sd_apps, 52, 50);

    s_gear = lv_obj_create(s_home);
    lv_obj_remove_style_all(s_gear);
    lv_obj_set_pos(s_gear, 732, 20);
    lv_obj_set_size(s_gear, 56, 56);
    lv_obj_add_flag(s_gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_gear, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_gear, lv_color_hex(0x16243D), 0);
    lv_obj_set_style_border_width(s_gear, 1, 0);
    lv_obj_set_style_border_color(s_gear, C_BORDER, 0);
    focus_style(s_gear, 14);
    lv_obj_t *gi = label(s_gear, &lv_font_montserrat_28, C_TEXT, LV_SYMBOL_SETTINGS);
    lv_obj_center(gi);
    lv_obj_add_event_cb(s_gear, on_gear_click, LV_EVENT_CLICKED, NULL);

    /* Applications */
    lv_obj_t *apps = label(s_home, &lv_font_montserrat_20, C_TEXT, "Applications");
    lv_obj_set_pos(apps, 20, 100);
    s_count = label(s_home, &lv_font_montserrat_16, lv_color_hex(0x6F8FC0), "");
    lv_obj_align(s_count, LV_ALIGN_TOP_RIGHT, -22, 103);

    s_grid = lv_obj_create(s_home);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_pos(s_grid, 10, 128);
    lv_obj_set_size(s_grid, 780, 292);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(s_grid, 6, 0);
    lv_obj_set_style_pad_column(s_grid, 8, 0);
    lv_obj_set_style_pad_row(s_grid, 6, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_ACTIVE);

    /* System bar */
    lv_obj_t *bar = panel(s_home, 16, 426, 768, 46, 12);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_sys_cpu = label(bar, &lv_font_montserrat_14, C_TEXT, "");
    s_sys_sram = label(bar, &lv_font_montserrat_14, C_TEXT, "");
    s_sys_psram = label(bar, &lv_font_montserrat_14, C_TEXT, "");
    s_sys_temp = label(bar, &lv_font_montserrat_14, C_TEXT, "");
    s_sys_pads = label(bar, &lv_font_montserrat_14, C_TEXT, "");
    s_sys_state = label(bar, &lv_font_montserrat_14, C_GREEN, LV_SYMBOL_OK " System ready");
}

static void refresh_home(void)
{
    scan_apps();
    build_grid();
    home_focus_list();
}

/* ── Settings screen ───────────────────────────────────────────────────── */

static void save_settings(void)
{
    nvs_handle_t h;
    if (nvs_open("launcher", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "volume", (uint8_t)s_volume);
    nvs_set_u8(h, "bright", (uint8_t)s_brightness);
    nvs_commit(h);
    nvs_close(h);
}

static void apply_settings(void)
{
    lx_audio_master_volume(s_volume);
    bsp_display_backlight(s_brightness);
    char buf[16];
    if (s_vol_val) {
        snprintf(buf, sizeof(buf), "%d%%", s_volume);
        lv_label_set_text(s_vol_val, buf);
        snprintf(buf, sizeof(buf), "%d%%", s_brightness);
        lv_label_set_text(s_bri_val, buf);
    }
}

static void on_slider(lv_event_t *ev)
{
    lv_obj_t *s = lv_event_get_target(ev);
    if (s == s_vol_slider) s_volume = lv_slider_get_value(s);
    else s_brightness = lv_slider_get_value(s);
    apply_settings();
    if (lv_event_get_code(ev) == LV_EVENT_RELEASED) save_settings();
}

static lv_obj_t *settings_row(lv_obj_t *parent, int y, const char *title, const char *symbol,
                              int min, int max, int value, lv_obj_t **val_out)
{
    lv_obj_t *row = panel(parent, 24, y, 752, 76, 14);
    lv_obj_t *ic = label(row, &lv_font_montserrat_24, C_ACCENT, symbol);
    lv_obj_set_pos(ic, 18, 24);
    lv_obj_t *t = label(row, &lv_font_montserrat_20, C_TEXT, title);
    lv_obj_set_pos(t, 60, 24);
    lv_obj_t *s = lv_slider_create(row);
    lv_obj_set_size(s, 420, 12);
    lv_obj_set_pos(s, 220, 31);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, C_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, C_TEXT, LV_PART_KNOB);
    lv_obj_set_style_outline_width(s, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(s, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(s, 8, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(s, on_slider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, on_slider, LV_EVENT_RELEASED, NULL);
    *val_out = label(row, &lv_font_montserrat_20, C_DIM, "");
    lv_obj_set_pos(*val_out, 668, 24);
    return s;
}

static lv_obj_t *s_back;
static void refresh_info(void);
static lv_obj_t *s_crash_box, *s_crash_ok;

static void on_crash_ok(lv_event_t *ev)
{
    lv_msgbox_close(s_crash_box);
    s_crash_box = s_crash_ok = NULL;
    s_focused = NULL;
    home_focus_list();
}

static void on_dev_switch(lv_event_t *ev)
{
    s_show_dev = lv_obj_has_state(s_dev_switch, LV_STATE_CHECKED);
    nvs_handle_t h;
    if (nvs_open("launcher", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "devapps", s_show_dev);
        nvs_commit(h);
        nvs_close(h);
    }
    scan_apps();                /* the grid is rebuilt when Settings closes */
    refresh_info();
}

static void build_settings(void)
{
    s_settings = lv_obj_create(NULL);
    lv_obj_remove_flag(s_settings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_settings, C_BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(s_settings, C_BG_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(s_settings, LV_GRAD_DIR_VER, 0);

    s_back = lv_obj_create(s_settings);
    lv_obj_remove_style_all(s_back);
    lv_obj_set_pos(s_back, 16, 14);
    lv_obj_set_size(s_back, 56, 56);
    lv_obj_add_flag(s_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_back, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_back, lv_color_hex(0x16243D), 0);
    focus_style(s_back, 14);
    lv_obj_t *bi = label(s_back, &lv_font_montserrat_28, C_TEXT, LV_SYMBOL_LEFT);
    lv_obj_center(bi);
    lv_obj_add_event_cb(s_back, on_back_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t = label(s_settings, &lv_font_montserrat_28, C_TEXT, "Settings");
    lv_obj_set_pos(t, 92, 26);

    s_vol_slider = settings_row(s_settings, 90, "Volume", LV_SYMBOL_VOLUME_MAX, 0, 100, s_volume, &s_vol_val);
    s_bri_slider = settings_row(s_settings, 178, "Brightness", LV_SYMBOL_IMAGE, 10, 100, s_brightness, &s_bri_val);

    lv_obj_t *dev = panel(s_settings, 24, 266, 752, 60, 14);
    lv_obj_t *di = label(dev, &lv_font_montserrat_24, C_ACCENT, LV_SYMBOL_LIST);
    lv_obj_set_pos(di, 18, 16);
    lv_obj_t *dt = label(dev, &lv_font_montserrat_20, C_TEXT, "Show developer apps");
    lv_obj_set_pos(dt, 60, 16);
    s_dev_switch = lv_switch_create(dev);
    lv_obj_set_pos(s_dev_switch, 664, 14);
    lv_obj_set_style_bg_color(s_dev_switch, C_BLUE, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_outline_width(s_dev_switch, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(s_dev_switch, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(s_dev_switch, 6, LV_STATE_FOCUSED);
    if (s_show_dev) lv_obj_add_state(s_dev_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_dev_switch, on_dev_switch, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *info = panel(s_settings, 24, 336, 752, 136, 14);
    s_info = label(info, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_set_pos(s_info, 18, 10);
    lv_obj_set_style_text_line_space(s_info, 6, 0);
    apply_settings();
}

static void refresh_info(void)
{
    const bsp_info_t *b = bsp_info();
    uint64_t total = 0, free_b = 0;
    esp_vfs_fat_info("/sd", &total, &free_b);
    char buf[512];
    snprintf(buf, sizeof(buf),
             "Board         %s\n"
             "Launcher      v%s   (app ABI %s, ESP-IDF %s)\n"
             "Memory        %u KB internal free,  %u KB PSRAM free\n"
             "SD card       %.1f GB free of %.1f GB,  %d apps\n"
             "Input         %s",
             b->name, LX_LAUNCHER_VERSION, LX_ABI_STRING, esp_get_idf_version(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             free_b / 1e9, total / 1e9, s_napps, "");
    size_t n = strlen(buf);
    lx_input_describe(buf + n, sizeof(buf) - n);
    if (!buf[n]) snprintf(buf + n, sizeof(buf) - n, "no gamepad or keyboard");
    lv_label_set_text(s_info, buf);
}

static void show_settings(void)
{
    refresh_info();
    lv_screen_load(s_settings);
    focus_clear_list();
    focus_add(s_back);
    focus_add(s_vol_slider);
    focus_add(s_bri_slider);
    focus_add(s_dev_switch);
    focus_set(s_vol_slider);
}

static void show_home(void)
{
    lv_screen_load(s_home);
    build_grid();
    s_focused = NULL;
    home_focus_list();
}

/* ── Periodic updates: clock, SD, system bar ───────────────────────────── */

static void update_status(lv_timer_t *t)
{
    static int tick;
    char buf[64];
    time_t now = time(NULL);
    if (now > 1700000000) {                     /* set (by DevLink, later RTC / NTP) */
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(buf, sizeof(buf), "%H:%M", &tm);
        lv_label_set_text(s_clock, buf);
        strftime(buf, sizeof(buf), "%a, %d %b %Y", &tm);
        lv_label_set_text(s_date, buf);
    } else {
        lv_label_set_text(s_clock, "--:--");
        lv_label_set_text(s_date, "Clock not set");
    }

    if (tick++ % 10 == 0) {
        uint64_t total = 0, free_b = 0;
        if (esp_vfs_fat_info("/sd", &total, &free_b) == ESP_OK) {
            snprintf(buf, sizeof(buf), "%.1f GB free", free_b / 1e9);
            lv_label_set_text(s_sd_free, buf);
        } else {
            lv_label_set_text(s_sd_free, "not mounted");
        }
        snprintf(buf, sizeof(buf), "%d app%s installed", s_napps, s_napps == 1 ? "" : "s");
        lv_label_set_text(s_sd_apps, buf);
    }

    snprintf(buf, sizeof(buf), "ESP32-P4  %d MHz x2", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    lv_label_set_text(s_sys_cpu, buf);
    snprintf(buf, sizeof(buf), "SRAM %u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    lv_label_set_text(s_sys_sram, buf);
    snprintf(buf, sizeof(buf), "PSRAM %.1f MB free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0);
    lv_label_set_text(s_sys_psram, buf);
    float c;
    if (s_tsens && temperature_sensor_get_celsius(s_tsens, &c) == ESP_OK) {
        snprintf(buf, sizeof(buf), "Chip %.0f C", c);
        lv_label_set_text(s_sys_temp, buf);
    }
    int pads = 0;
    for (int i = 0; i < PAPP_MAX_PLAYERS; i++) {
        papp_gamepad_t g;
        pads += lx_input_gamepad(i, &g);
    }
    snprintf(buf, sizeof(buf), LV_SYMBOL_USB " %d pad%s", pads, pads == 1 ? "" : "s");
    lv_label_set_text(s_sys_pads, buf);
    if (lv_screen_active() == s_settings && tick % 2 == 0) refresh_info();
}

/* After a crash reboot: say which app crashed (details: papp_dev.py crash) */
static void show_crash_notice(void)
{
    lx_app_crash_ctx_t c;
    if (!lx_app_last_crash(&c)) return;
    papp_manifest_t m;
    const char *name = lx_app_read_manifest(c.path, &m) == ESP_OK ? m.name : c.path;
    char text[256];
    snprintf(text, sizeof(text), "%s stopped with an error and the board restarted.\n"
             "Details for developers: papp_dev.py crash", name);
    lv_obj_t *mb = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mb, LV_SYMBOL_WARNING "  App crashed");
    lv_msgbox_add_text(mb, text);
    lv_obj_t *ok = lv_msgbox_add_footer_button(mb, "OK");
    lv_obj_add_event_cb(ok, on_crash_ok, LV_EVENT_CLICKED, mb);
    lv_obj_set_width(mb, 520);
    s_crash_box = mb;
    s_crash_ok = ok;
    focus_set(ok);
}

/* ── LVGL glue ─────────────────────────────────────────────────────────── */

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
    if (lv_display_flush_is_last(d) && lx_app_state() == LX_APP_IDLE)
        lx_comp_launcher_present(s_frame);
    lv_display_flush_ready(d);
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    bsp_touch_point_t p[1];
    if ((bsp_info()->caps & BSP_CAP_TOUCH) && bsp_touch_read(p, 1) > 0) {
        data->point.x = p[0].x;
        data->point.y = p[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* Gamepad and keyboard: directions move the focus; on a slider left/right
 * change its value; A / Start / Enter click; B / Esc go back. */
static void poll_keys(void)
{
    papp_input_event_t ev;
    while (lx_input_poll_event(&ev)) {
        int dx = 0, dy = 0, ok = 0, back = 0;
        if (ev.type == PAPP_EV_BUTTON_DOWN) {
            switch (ev.code) {
            case 16: dy = -1; break;
            case 17: dy = 1; break;
            case 18: dx = -1; break;
            case 19: dx = 1; break;
            case 0: case 9: ok = 1; break;          /* A, START */
            case 1: back = 1; break;                /* B */
            default: break;
            }
        } else if (ev.type == PAPP_EV_KEY_DOWN) {
            switch (ev.code) {
            case 0x52: dy = -1; break;
            case 0x51: dy = 1; break;
            case 0x50: dx = -1; break;
            case 0x4F: dx = 1; break;
            case 0x28: case 0x58: case 0x2C: ok = 1; break;   /* Enter, keypad Enter, Space */
            case 0x29: case 0x2A: back = 1; break;            /* Esc, Backspace */
            default: break;
            }
        } else {
            continue;
        }
        if (s_crash_box) {
            if (ok || back) lv_obj_send_event(s_crash_ok, LV_EVENT_CLICKED, NULL);
            continue;
        }
        if (dx && s_focused && lv_obj_check_type(s_focused, &lv_slider_class)) {
            lv_slider_set_value(s_focused, lv_slider_get_value(s_focused) + dx * 5, LV_ANIM_OFF);
            lv_obj_send_event(s_focused, LV_EVENT_VALUE_CHANGED, NULL);
            save_settings();
        } else if (dx || dy) {
            focus_move(dx, dy);
        } else if (ok && s_focused) {
            lv_obj_send_event(s_focused, LV_EVENT_CLICKED, NULL);
        } else if (back && lv_screen_active() == s_settings) {
            show_home();
        }
    }
}

static void launch(const char *path)
{
    char err[160];
    remember_last(path);
    ESP_LOGI(TAG, "starting %s", path);
    if (lx_app_launch(path, 0, NULL, err, sizeof(err)) != ESP_OK) {
        lv_obj_t *mb = lv_msgbox_create(NULL);
        lv_msgbox_add_title(mb, "Cannot start the app");
        lv_msgbox_add_text(mb, err);
        lv_msgbox_add_close_button(mb);
    }
}

static void home_task(void *arg)
{
    bool was_running = false;
    for (;;) {
        if (lx_app_state() != LX_APP_IDLE) {
            if (!was_running) {
                char p[128];                              /* also apps started over DevLink */
                snprintf(p, sizeof(p), "%s", lx_app_path());
                if (p[0] && strcmp(p, s_last) != 0) remember_last(p);
            }
            was_running = true;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (was_running) {                      /* back from an app: redraw everything */
            was_running = false;
            lx_input_flush();
            s_volume = lx_audio_master_volume_get();
            lv_slider_set_value(s_vol_slider, s_volume, LV_ANIM_OFF);
            apply_settings();
            if (lv_screen_active() != s_home) lv_screen_load(s_home);
            s_focused = NULL;
            refresh_home();
            lv_obj_invalidate(lv_screen_active());
        }
        poll_keys();
        uint32_t wait = lv_timer_handler();
        if (s_launch[0]) {
            char path[160];
            snprintf(path, sizeof(path), "%s", s_launch);
            s_launch[0] = 0;
            launch(path);
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(wait < 5 ? 5 : wait > 20 ? 20 : wait));
    }
}

esp_err_t home_start(void)
{
    nvs_handle_t h;
    if (nvs_open("launcher", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "volume", &v) == ESP_OK) s_volume = v;
        if (nvs_get_u8(h, "bright", &v) == ESP_OK) s_brightness = v;
        if (nvs_get_u8(h, "devapps", &v) == ESP_OK) s_show_dev = v;
        nvs_close(h);
    }

    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&tc, &s_tsens) == ESP_OK) temperature_sensor_enable(s_tsens);
    else s_tsens = NULL;

    int w, h_px;
    lx_comp_display_size(&w, &h_px);
    size_t bytes = (size_t)w * h_px * 2;
    s_frame = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM);
    if (!s_frame) return ESP_ERR_NO_MEM;

    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(w, h_px);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, s_frame, NULL, bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read);

    build_home();
    build_settings();
    refresh_home();
    lv_screen_load(s_home);
    lv_timer_create(update_status, 1000, NULL);
    update_status(NULL);
    show_crash_notice();

    xTaskCreatePinnedToCore(home_task, "home", 8192, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "home screen up, %d apps", s_napps);
    return ESP_OK;
}
