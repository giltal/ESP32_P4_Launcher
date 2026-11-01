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
 *
 * Folders: a directory in /sd/apps with no .papp in it, holding app directories
 * (/sd/apps/<folder>/<dir>/<file>.papp), one level only. X / Y, or holding a tile,
 * opens its menu: move an app, rename or delete a folder.
 */
#include "home.h"
#include "bsp.h"
#include "lx_app.h"
#include "lx_audio.h"
#include "lx_comp.h"
#include "lx_common.h"
#include "lx_input.h"
#include "lx_update.h"
#include "lx_net.h"
#include "esp_system.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
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
#define MAX_APPS      64           /* apps + folders */
#define MAX_NAME      24           /* folder name */
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
    char path[160];             /* .papp, or the directory of a folder */
    char icon[176];             /* "S:/sd/apps/x/icon.png" or "" */
    char name[32];
    char id[32];
    char parent[MAX_NAME + 8];  /* folder it is in, "" = main page */
    bool hidden;                /* PAPP_MF_HIDDEN */
    bool is_folder;
} app_entry_t;

static app_entry_t s_apps[MAX_APPS];
static int s_napps;             /* entries in s_apps */
static int s_ninstalled;        /* of them, apps */
static char s_folder[MAX_NAME + 8];     /* folder on screen, "" = main page */
static char s_files_app[160];   /* the Files app: opened by the SD card panel, not a tile */
static char s_last[160];        /* last launched .papp */

static lv_display_t *s_disp;
static uint16_t *s_frame;
static lv_obj_t *s_home, *s_settings;
static lv_obj_t *s_clock, *s_date, *s_sd_free, *s_sd_apps, *s_grid, *s_count, *s_title;
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
    const app_entry_t *x = a, *y = b;
    if (x->is_folder != y->is_folder) return y->is_folder - x->is_folder;
    return strcasecmp(x->name, y->name);
}

/* Adds the app in dir; false if there is no .papp in it */
static bool scan_app(const char *dir, const char *parent)
{
    bool found = false;
    DIR *sd = opendir(dir);
    if (!sd) return true;
    struct dirent *fe;
    while (s_napps < MAX_APPS && (fe = readdir(sd))) {
        size_t n = strlen(fe->d_name);
        if (n < 6 || strcasecmp(fe->d_name + n - 5, ".papp") != 0) continue;
        app_entry_t *e = &s_apps[s_napps];
        memset(e, 0, sizeof(*e));
        snprintf(e->parent, sizeof(e->parent), "%s", parent);
        snprintf(e->path, sizeof(e->path), "%s/%s", dir, fe->d_name);
        papp_manifest_t m;
        if (lx_app_read_manifest(e->path, &m) == ESP_OK) {
            snprintf(e->name, sizeof(e->name), "%s", m.name);
            snprintf(e->id, sizeof(e->id), "%s", m.id);
            e->hidden = m.flags & PAPP_MF_HIDDEN;
            if (strcmp(m.id, "tool.files") == 0) {
                snprintf(s_files_app, sizeof(s_files_app), "%s", e->path);
                found = true;
                break;
            }
        } else {
            snprintf(e->name, sizeof(e->name), "%.*s", (int)(n - 5), fe->d_name);
        }
        char icon[160];
        struct stat st;
        snprintf(icon, sizeof(icon), "%s/icon.png", dir);
        if (stat(icon, &st) == 0) snprintf(e->icon, sizeof(e->icon), "S:%s", icon);
        if (!e->hidden || s_show_dev) {
            s_napps++;
            s_ninstalled++;
        }
        found = true;
        break;                                      /* one app per directory */
    }
    closedir(sd);
    return found;
}

static void scan_dir(const char *base, const char *parent)
{
    DIR *d = opendir(base);
    if (!d) return;
    struct dirent *de;
    while (s_napps < MAX_APPS && (de = readdir(d))) {
        if (de->d_name[0] == '.' || de->d_type != DT_DIR) continue;
        char dir[128];
        snprintf(dir, sizeof(dir), "%s/%s", base, de->d_name);
        if (scan_app(dir, parent) || parent[0] || s_napps >= MAX_APPS) continue;
        app_entry_t *e = &s_apps[s_napps++];        /* no app in it: a folder */
        memset(e, 0, sizeof(*e));
        e->is_folder = true;
        snprintf(e->path, sizeof(e->path), "%s", dir);
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        scan_dir(dir, de->d_name);
    }
    closedir(d);
}

static void scan_apps(void)
{
    s_napps = s_ninstalled = 0;
    s_files_app[0] = 0;
    scan_dir(APPS_DIR, "");
    bool here = false;
    for (int i = 0; i < s_napps; i++) here |= s_apps[i].is_folder && strcmp(s_apps[i].name, s_folder) == 0;
    if (!here) s_folder[0] = 0;
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
    uint32_t c = e && e->is_folder ? 0xD9921A : TILE_COLORS[hash % (sizeof(TILE_COLORS) / sizeof(TILE_COLORS[0]))];
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_lighten(lv_color_hex(c), 40), 0);
    lv_obj_set_style_bg_grad_color(box, lv_color_hex(c), 0);
    lv_obj_set_style_bg_grad_dir(box, LV_GRAD_DIR_VER, 0);
    lv_obj_t *l = lv_label_create(box);
    char initial[2] = { e && e->name[0] ? (char)toupper((unsigned char)e->name[0]) : '?', 0 };
    lv_label_set_text(l, e && e->is_folder ? LV_SYMBOL_DIRECTORY : initial);
    lv_obj_set_style_text_font(l, size >= 80 ? &lv_font_montserrat_48 : &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, C_TEXT, 0);
    lv_obj_center(l);
    return box;
}

/* ── Clicks ────────────────────────────────────────────────────────────── */

static bool s_long_press;        /* the click ending a long press is not a click */
static void folder_enter(const char *name);
static void tile_menu(const app_entry_t *e);

static void on_tile_click(lv_event_t *ev)
{
    const app_entry_t *e = lv_event_get_user_data(ev);
    if (s_long_press) s_long_press = false;
    else if (e->is_folder) folder_enter(e->name);
    else snprintf(s_launch, sizeof(s_launch), "%s", e->path);
}

static void on_tile_long(lv_event_t *ev)
{
    s_long_press = true;
    tile_menu(lv_event_get_user_data(ev));
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
    int shown = 0;
    for (int i = 0; i < s_napps; i++) {
        app_entry_t *e = &s_apps[i];
        if (strcmp(e->parent, s_folder) != 0) continue;
        shown++;
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
        lv_obj_set_user_data(t, e);
        lv_obj_add_event_cb(t, on_tile_click, LV_EVENT_CLICKED, e);
        lv_obj_add_event_cb(t, on_tile_long, LV_EVENT_LONG_PRESSED, e);
    }
    if (!shown) {
        lv_obj_t *l = label(s_grid, &lv_font_montserrat_16, C_DIM, s_folder[0] ?
                            "This folder is empty. To add an app, hold it or press X on the main page." :
                            "No apps on the SD card yet (put them in /sd/apps/<name>/<name>.papp)");
        lv_obj_set_style_pad_top(l, 40, 0);
    }
    if (s_folder[0]) {
        char buf[48];
        snprintf(buf, sizeof(buf), LV_SYMBOL_DIRECTORY "  %s", s_folder);
        lv_label_set_text(s_title, buf);
        lv_label_set_text(s_count, LV_SYMBOL_LEFT " Back");
    } else {
        lv_label_set_text(s_title, "Applications");
        lv_label_set_text(s_count, LV_SYMBOL_PLUS " New folder");
    }
}

static lv_obj_t *s_gear, *s_sd;

static void home_focus_list(void)
{
    lv_obj_t *keep = s_focused;
    focus_clear_list();
    lv_obj_remove_state(s_gear, LV_STATE_FOCUSED);      /* s_focused may have been reset */
    lv_obj_remove_state(s_count, LV_STATE_FOCUSED);
    lv_obj_remove_state(s_sd, LV_STATE_FOCUSED);
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_grid); i++) {
        lv_obj_t *c = lv_obj_get_child(s_grid, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE)) focus_add(c);
    }
    focus_add(s_gear);
    focus_add(s_sd);
    focus_add(s_count);
    bool still_there = false;
    for (int i = 0; i < s_nfocus; i++) still_there |= s_focusable[i] == keep;
    s_focused = NULL;
    focus_set(still_there ? keep : (s_nfocus ? s_focusable[0] : NULL));
    if (!still_there) lv_obj_scroll_to_y(s_grid, 0, LV_ANIM_OFF);
}

/* Redraws the grid and puts the focus on the entry with this path */
static void home_show(const char *focus)
{
    if (s_focused) lv_obj_remove_state(s_focused, LV_STATE_FOCUSED);
    s_focused = NULL;
    build_grid();
    for (uint32_t i = 0; focus && i < lv_obj_get_child_count(s_grid); i++) {
        lv_obj_t *c = lv_obj_get_child(s_grid, i);
        const app_entry_t *e = lv_obj_get_user_data(c);
        if (e && strcmp(e->path, focus) == 0) s_focused = c;
    }
    home_focus_list();
}

static void home_view(const char *focus)
{
    scan_apps();
    home_show(focus);
}

static void on_count_click(lv_event_t *ev);
static void on_sd_click(lv_event_t *ev);

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

    lv_obj_t *sd = s_sd = panel(s_home, 506, 10, 214, 76, 14);
    lv_obj_add_flag(sd, LV_OBJ_FLAG_CLICKABLE);
    focus_style(sd, 14);
    lv_obj_set_style_bg_color(sd, C_FOCUS, LV_STATE_PRESSED);
    lv_obj_add_event_cb(sd, on_sd_click, LV_EVENT_CLICKED, NULL);
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
    s_title = label(s_home, &lv_font_montserrat_20, C_TEXT, "Applications");
    lv_obj_set_pos(s_title, 20, 100);
    s_count = label(s_home, &lv_font_montserrat_16, lv_color_hex(0x6F8FC0), "");
    lv_obj_align(s_count, LV_ALIGN_TOP_RIGHT, -22, 103);
    lv_obj_add_flag(s_count, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_count, 12);
    focus_style(s_count, 6);
    lv_obj_add_event_cb(s_count, on_count_click, LV_EVENT_CLICKED, NULL);

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
    lv_obj_t *row = panel(parent, 24, y, 752, 64, 14);
    lv_obj_t *ic = label(row, &lv_font_montserrat_24, C_ACCENT, symbol);
    lv_obj_set_pos(ic, 18, 18);
    lv_obj_t *t = label(row, &lv_font_montserrat_20, C_TEXT, title);
    lv_obj_set_pos(t, 60, 20);
    lv_obj_t *s = lv_slider_create(row);
    lv_obj_set_size(s, 420, 12);
    lv_obj_set_pos(s, 220, 26);
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
    lv_obj_set_pos(*val_out, 668, 20);
    return s;
}

static lv_obj_t *s_back;
static void refresh_info(void);
static void upd_message(const char *title, const char *text);
static void show_settings(void);

/* ── Controller mapping ──────────────────────────────────────── */

#define MAP_STEP_US 6000000             /* nothing pressed for this long: skip the step */

static const struct { const char *name; uint8_t bit; } k_map_steps[] = {
    { "D-pad UP", 16 }, { "D-pad DOWN", 17 }, { "D-pad LEFT", 18 }, { "D-pad RIGHT", 19 },
    { "A", 0 }, { "B", 1 }, { "X", 2 }, { "Y", 3 }, { "L1", 4 }, { "R1", 5 }, { "L2", 6 },
    { "R2", 7 }, { "SELECT", 8 }, { "START", 9 }, { "L3", 10 }, { "R3", 11 },
};
#define MAP_STEPS ((int)(sizeof(k_map_steps) / sizeof(k_map_steps[0])))

static lv_obj_t *s_map_btn, *s_map_scr, *s_map_prompt, *s_map_hint, *s_map_bar;
static int s_map_step = -1, s_map_player, s_map_done;
static uint8_t s_map[LX_INPUT_MAP_BITS];
static bool s_map_armed;                /* all released since the last step */
static int64_t s_map_since;

static void map_show_step(void)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "Press %s", k_map_steps[s_map_step].name);
    lv_label_set_text(s_map_prompt, buf);
    snprintf(buf, sizeof(buf), "Step %d of %d. No such button? Wait a moment.", s_map_step + 1, MAP_STEPS);
    lv_label_set_text(s_map_hint, buf);
    lv_bar_set_value(s_map_bar, s_map_step, LV_ANIM_OFF);
    s_map_since = esp_timer_get_time();
}

static void map_finish(void)
{
    s_map_step = -1;
    if (s_map_done >= 4) {
        /* Buttons not assigned keep their default, unless that function moved */
        for (int i = 0; i < LX_INPUT_MAP_BITS; i++) {
            if (s_map[i] != 0xFF) continue;
            bool taken = false;
            for (int j = 0; j < LX_INPUT_MAP_BITS; j++) taken |= s_map[j] == i;
            if (!taken) s_map[i] = (uint8_t)i;
        }
        lx_input_pad_set_map(s_map_player, s_map);
    }
    lx_input_flush();
    show_settings();
    if (s_map_done < 4) upd_message("Controller", "Fewer than 4 buttons pressed, nothing was changed.");
    else upd_message("Controller", "Mapping saved for this controller.");
}

static void map_tick(void)
{
    uint32_t raw = lx_input_pad_raw(s_map_player);
    lx_input_flush();
    if (!raw) s_map_armed = true;
    int64_t now = esp_timer_get_time();
    bool next = now - s_map_since > MAP_STEP_US;
    if (s_map_armed && raw) {
        int bit = __builtin_ctz(raw);
        if (bit < LX_INPUT_MAP_BITS && s_map[bit] == 0xFF) {
            s_map[bit] = k_map_steps[s_map_step].bit;
            s_map_done++;
            next = true;
        }
        s_map_armed = false;
    }
    if (!next) return;
    if (++s_map_step >= MAP_STEPS) map_finish();
    else map_show_step();
}

static void on_map_start(lv_event_t *ev)
{
    s_map_player = lx_input_pad_first();
    if (s_map_player < 0) {
        upd_message("Controller", "Connect a USB controller first.");
        return;
    }
    memset(s_map, 0xFF, sizeof(s_map));
    s_map_done = 0;
    s_map_armed = false;
    s_map_step = 0;
    focus_clear_list();
    lv_screen_load(s_map_scr);
    map_show_step();
}

static void on_map_cancel(lv_event_t *ev)
{
    s_map_step = -1;
    lx_input_flush();
    show_settings();
}

static void on_map_reset(lv_event_t *ev)
{
    s_map_step = -1;
    lx_input_pad_set_map(s_map_player, NULL);
    lx_input_flush();
    show_settings();
    upd_message("Controller", "This controller is back to its default buttons.");
}

static void build_map_screen(void)
{
    s_map_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_map_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_map_scr, C_BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(s_map_scr, C_BG_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(s_map_scr, LV_GRAD_DIR_VER, 0);
    lv_obj_t *t = label(s_map_scr, &lv_font_montserrat_28, C_TEXT, "Map controller");
    lv_obj_set_pos(t, 32, 26);
    s_map_prompt = label(s_map_scr, &lv_font_montserrat_48, C_ACCENT, "");
    lv_obj_align(s_map_prompt, LV_ALIGN_CENTER, 0, -40);
    s_map_hint = label(s_map_scr, &lv_font_montserrat_20, C_DIM, "");
    lv_obj_align(s_map_hint, LV_ALIGN_CENTER, 0, 20);
    s_map_bar = lv_bar_create(s_map_scr);
    lv_obj_set_size(s_map_bar, 600, 12);
    lv_obj_align(s_map_bar, LV_ALIGN_CENTER, 0, 70);
    lv_bar_set_range(s_map_bar, 0, MAP_STEPS);
    lv_obj_set_style_bg_color(s_map_bar, C_BLUE, LV_PART_INDICATOR);
    static const struct { const char *text; int x; lv_event_cb_t cb; } b[] = {
        { "Cancel", 200, on_map_cancel }, { "Reset to default", 420, on_map_reset },
    };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *btn = lv_button_create(s_map_scr);
        lv_obj_set_pos(btn, b[i].x, 390);
        lv_obj_set_style_bg_color(btn, C_BLUE, 0);
        lv_obj_center(label(btn, &lv_font_montserrat_20, C_TEXT, b[i].text));
        lv_obj_add_event_cb(btn, b[i].cb, LV_EVENT_CLICKED, NULL);
    }
}

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

/* ── Wi-Fi screens ───────────────────────────────────────────── */

static lv_obj_t *s_wifi_btn, *s_wifi_scr, *s_wifi_status, *s_wifi_list, *s_wifi_tz;
static lv_obj_t *s_wifi_scan, *s_wifi_forget, *s_wifi_back, *s_pw_scr, *s_pw_title, *s_pw_text;
static bool s_wifi_scanning;
enum { PW_WIFI, PW_NEW_FOLDER, PW_RENAME };     /* s_pw_scr also asks for folder names */
static int s_pw_mode;
static void folder_name_done(const char *name);
static lx_net_ap_t s_wifi_aps[LX_NET_MAX_APS];
static int s_wifi_sel;

static void wifi_focus(void)
{
    focus_clear_list();
    focus_add(s_wifi_back);
    focus_add(s_wifi_scan);
    focus_add(s_wifi_forget);
    focus_add(s_wifi_tz);
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_wifi_list); i++)
        focus_add(lv_obj_get_child(s_wifi_list, i));
    s_focused = NULL;
    focus_set(s_wifi_scan);
}

static void on_wifi_scan(lv_event_t *ev)
{
    s_wifi_scanning = true;
    lv_obj_clean(s_wifi_list);
    lx_net_scan();
}

static void on_wifi_open(lv_event_t *ev)
{
    lv_screen_load(s_wifi_scr);
    wifi_focus();
    on_wifi_scan(NULL);
}

static void on_wifi_back(lv_event_t *ev) { show_settings(); }
static void on_wifi_forget(lv_event_t *ev) { lx_net_forget(); }

static void on_wifi_tz(lv_event_t *ev)
{
    lx_net_tz_set(lv_dropdown_get_selected(s_wifi_tz));
}

static void on_wifi_pick(lv_event_t *ev)
{
    s_wifi_sel = (int)(intptr_t)lv_event_get_user_data(ev);
    if (!s_wifi_aps[s_wifi_sel].secure) {
        lx_net_connect(s_wifi_aps[s_wifi_sel].ssid, "");
        return;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "Password for %s", s_wifi_aps[s_wifi_sel].ssid);
    s_pw_mode = PW_WIFI;
    lv_label_set_text(s_pw_title, buf);
    lv_textarea_set_password_mode(s_pw_text, true);
    lv_textarea_set_max_length(s_pw_text, 63);
    lv_textarea_set_text(s_pw_text, "");
    lv_screen_load(s_pw_scr);
    focus_clear_list();
}

static void pw_close(void)
{
    lv_textarea_set_text(s_pw_text, "");
    if (s_pw_mode == PW_WIFI) {
        lv_screen_load(s_wifi_scr);
        wifi_focus();
    } else {
        lv_screen_load(s_home);
        s_focused = NULL;
        home_focus_list();
    }
}

static void pw_accept(void)
{
    char text[64];
    snprintf(text, sizeof(text), "%s", lv_textarea_get_text(s_pw_text));
    if (s_pw_mode == PW_WIFI) lx_net_connect(s_wifi_aps[s_wifi_sel].ssid, text);
    pw_close();
    if (s_pw_mode != PW_WIFI) folder_name_done(text);
}

static void on_pw_key(lv_event_t *ev)
{
    if (lv_event_get_code(ev) == LV_EVENT_READY) pw_accept();
    else pw_close();
}

static lv_obj_t *wifi_button(lv_obj_t *parent, int x, int y, int w, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, 44);
    lv_obj_set_style_bg_color(b, C_BLUE, 0);
    lv_obj_set_style_outline_width(b, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(b, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(b, 3, LV_STATE_FOCUSED);
    lv_obj_center(label(b, &lv_font_montserrat_20, C_TEXT, text));
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void build_wifi_screens(void)
{
    s_wifi_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_wifi_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_wifi_scr, C_BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(s_wifi_scr, C_BG_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(s_wifi_scr, LV_GRAD_DIR_VER, 0);
    s_wifi_back = wifi_button(s_wifi_scr, 16, 14, 56, LV_SYMBOL_LEFT, on_wifi_back);
    lv_obj_t *t = label(s_wifi_scr, &lv_font_montserrat_28, C_TEXT, "Wi-Fi and time");
    lv_obj_set_pos(t, 92, 20);
    s_wifi_status = label(s_wifi_scr, &lv_font_montserrat_16, C_DIM, "");
    lv_obj_set_pos(s_wifi_status, 24, 72);
    s_wifi_scan = wifi_button(s_wifi_scr, 24, 104, 120, "Scan", on_wifi_scan);
    s_wifi_forget = wifi_button(s_wifi_scr, 160, 104, 200, "Forget network", on_wifi_forget);
    lv_obj_t *z = label(s_wifi_scr, &lv_font_montserrat_16, C_TEXT, "Time zone");
    lv_obj_set_pos(z, 390, 116);
    s_wifi_tz = lv_dropdown_create(s_wifi_scr);
    lv_obj_set_pos(s_wifi_tz, 484, 104);
    lv_obj_set_width(s_wifi_tz, 292);
    lv_dropdown_clear_options(s_wifi_tz);
    for (int i = 0; i < lx_net_tz_count(); i++)
        lv_dropdown_add_option(s_wifi_tz, lx_net_tz_name(i), LV_DROPDOWN_POS_LAST);
    lv_dropdown_set_selected(s_wifi_tz, lx_net_tz_get());
    lv_obj_set_style_outline_width(s_wifi_tz, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(s_wifi_tz, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(s_wifi_tz, on_wifi_tz, LV_EVENT_VALUE_CHANGED, NULL);
    s_wifi_list = lv_list_create(s_wifi_scr);
    lv_obj_set_pos(s_wifi_list, 24, 160);
    lv_obj_set_size(s_wifi_list, 752, 306);
    lv_obj_set_style_bg_color(s_wifi_list, C_PANEL, 0);
    lv_obj_set_style_border_color(s_wifi_list, C_BORDER, 0);

    s_pw_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_pw_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_pw_scr, C_BG_TOP, 0);
    s_pw_title = label(s_pw_scr, &lv_font_montserrat_20, C_TEXT, "");
    lv_obj_set_pos(s_pw_title, 24, 16);
    s_pw_text = lv_textarea_create(s_pw_scr);
    lv_obj_set_pos(s_pw_text, 24, 52);
    lv_obj_set_size(s_pw_text, 752, 52);
    lv_textarea_set_one_line(s_pw_text, true);
    lv_textarea_set_password_mode(s_pw_text, true);
    lv_textarea_set_max_length(s_pw_text, 63);
    lv_obj_t *kb = lv_keyboard_create(s_pw_scr);
    lv_obj_set_size(kb, 800, 360);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_pw_text);
    lv_obj_add_event_cb(kb, on_pw_key, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, on_pw_key, LV_EVENT_CANCEL, NULL);
}

static void wifi_tick(void)
{
    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (lv_screen_active() != s_wifi_scr || now - last < 300000) return;
    last = now;
    char buf[96];
    switch (lx_net_state()) {
    case LX_NET_CONNECTED:
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " Connected to %s, %s", lx_net_ssid(), lx_net_ip());
        break;
    case LX_NET_CONNECTING:
        snprintf(buf, sizeof(buf), "Connecting to %s...", lx_net_ssid());
        break;
    case LX_NET_SCANNING: snprintf(buf, sizeof(buf), "Scanning..."); break;
    case LX_NET_FAILED:
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " Could not join %s. Check the password.", lx_net_ssid());
        break;
    case LX_NET_NO_RADIO: snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " The Wi-Fi chip did not answer."); break;
    case LX_NET_IDLE: snprintf(buf, sizeof(buf), "Not connected"); break;
    default: snprintf(buf, sizeof(buf), s_wifi_scanning ? "Starting the radio..." : "Radio off"); break;
    }
    lv_label_set_text(s_wifi_status, buf);
    if (!s_wifi_scanning) return;
    int n = lx_net_scan_results(s_wifi_aps, LX_NET_MAX_APS);
    if (n < 0) return;
    s_wifi_scanning = false;
    for (int i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf), "%s  (%d dBm%s)", s_wifi_aps[i].ssid, s_wifi_aps[i].rssi,
                 s_wifi_aps[i].secure ? ", password" : ", open");
        lv_obj_t *b = lv_list_add_button(s_wifi_list, LV_SYMBOL_WIFI, buf);
        lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(b, C_ACCENT, LV_STATE_FOCUSED);
        lv_obj_add_event_cb(b, on_wifi_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (!n) lv_list_add_text(s_wifi_list, "No networks found");
    wifi_focus();
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

    s_vol_slider = settings_row(s_settings, 80, "Volume", LV_SYMBOL_VOLUME_MAX, 0, 100, s_volume, &s_vol_val);
    s_bri_slider = settings_row(s_settings, 150, "Brightness", LV_SYMBOL_IMAGE, 10, 100, s_brightness, &s_bri_val);

    lv_obj_t *dev = panel(s_settings, 24, 220, 752, 52, 14);
    lv_obj_t *di = label(dev, &lv_font_montserrat_24, C_ACCENT, LV_SYMBOL_LIST);
    lv_obj_set_pos(di, 18, 13);
    lv_obj_t *dt = label(dev, &lv_font_montserrat_20, C_TEXT, "Show developer apps");
    lv_obj_set_pos(dt, 60, 14);
    s_dev_switch = lv_switch_create(dev);
    lv_obj_set_pos(s_dev_switch, 664, 10);
    lv_obj_set_style_bg_color(s_dev_switch, C_BLUE, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_outline_width(s_dev_switch, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(s_dev_switch, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(s_dev_switch, 6, LV_STATE_FOCUSED);
    if (s_show_dev) lv_obj_add_state(s_dev_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_dev_switch, on_dev_switch, LV_EVENT_VALUE_CHANGED, NULL);
    s_map_btn = lv_button_create(s_settings);
    lv_obj_set_pos(s_map_btn, 24, 278);
    lv_obj_set_size(s_map_btn, 370, 52);
    lv_obj_set_style_bg_color(s_map_btn, C_BLUE, 0);
    lv_obj_set_style_outline_width(s_map_btn, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(s_map_btn, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(s_map_btn, 3, LV_STATE_FOCUSED);
    lv_obj_center(label(s_map_btn, &lv_font_montserrat_20, C_TEXT, "Map controller"));
    lv_obj_add_event_cb(s_map_btn, on_map_start, LV_EVENT_CLICKED, NULL);
    build_map_screen();
    s_wifi_btn = lv_button_create(s_settings);
    lv_obj_set_pos(s_wifi_btn, 406, 278);
    lv_obj_set_size(s_wifi_btn, 370, 52);
    lv_obj_set_style_bg_color(s_wifi_btn, C_BLUE, 0);
    lv_obj_set_style_outline_width(s_wifi_btn, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(s_wifi_btn, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(s_wifi_btn, 3, LV_STATE_FOCUSED);
    lv_obj_center(label(s_wifi_btn, &lv_font_montserrat_20, C_TEXT, LV_SYMBOL_WIFI " Wi-Fi"));
    lv_obj_add_event_cb(s_wifi_btn, on_wifi_open, LV_EVENT_CLICKED, NULL);
    build_wifi_screens();

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
             "SD card       %.1f GB free of %.1f GB,  %d apps   Battery pin %.2f V\n"
             "Input         %s",
             b->name, LX_LAUNCHER_VERSION, LX_ABI_STRING, esp_get_idf_version(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             free_b / 1e9, total / 1e9, s_ninstalled, bsp_battery_mv() / 1000.0, "");
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
    focus_add(s_map_btn);
    focus_add(s_wifi_btn);
    focus_set(s_vol_slider);
}

static void show_home(void)
{
    if (s_focused) lv_obj_remove_state(s_focused, LV_STATE_FOCUSED);    /* else it stays lit on Settings */
    lv_screen_load(s_home);
    build_grid();
    s_focused = NULL;
    home_focus_list();
}

/* ── Folders ───────────────────────────────────────────────────────────── */

enum { ACT_CLOSE, ACT_OPEN, ACT_RENAME, ACT_DELETE, ACT_DELETE_YES, ACT_PICK, ACT_OUT, ACT_NEW_MOVE, ACT_TO_FOLDER };

static lv_obj_t *s_menu, *s_menu_return;
static char s_target[160], s_target_name[32];   /* what the menu is about */
static bool s_name_move;                        /* move s_target into the folder being named */

static void menu_close(void)
{
    lv_msgbox_close(s_menu);
    s_menu = NULL;
    s_long_press = false;
    s_focused = s_menu_return;
    home_focus_list();
}

static void menu_open(const char *title, const char *text, int items)
{
    if (s_focused) lv_obj_remove_state(s_focused, LV_STATE_FOCUSED);
    s_menu_return = s_focused;
    focus_clear_list();
    s_menu = lv_msgbox_create(NULL);
    lv_msgbox_add_title(s_menu, title);
    lv_obj_set_width(s_menu, 460);
    if (text) lv_msgbox_add_text(s_menu, text);
    if (items > 5) lv_obj_set_height(lv_msgbox_get_content(s_menu), 5 * 54);
}

static void on_menu_item(lv_event_t *ev);

static void menu_add(const char *text, int act)
{
    lv_obj_t *b = lv_button_create(lv_msgbox_get_content(s_menu));
    lv_obj_set_size(b, lv_pct(100), 44);
    lv_obj_set_style_bg_color(b, C_BLUE, 0);
    lv_obj_set_style_outline_width(b, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(b, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_center(label(b, &lv_font_montserrat_20, C_TEXT, text));
    lv_obj_add_event_cb(b, on_menu_item, LV_EVENT_CLICKED, (void *)(intptr_t)act);
    focus_add(b);
    if (s_nfocus == 1) focus_set(b);
}

static void folder_message(const char *title, const char *text)
{
    menu_open(title, text, 1);
    menu_add("OK", ACT_CLOSE);
}

static void folder_enter(const char *name)
{
    snprintf(s_folder, sizeof(s_folder), "%s", name);
    home_show(NULL);
}

static void folder_leave(void)
{
    char path[64];
    snprintf(path, sizeof(path), APPS_DIR "/%s", s_folder);
    s_folder[0] = 0;
    home_show(path);
}

static void name_open(const char *title, const char *text, int mode)
{
    s_pw_mode = mode;
    lv_label_set_text(s_pw_title, title);
    lv_textarea_set_password_mode(s_pw_text, false);
    lv_textarea_set_max_length(s_pw_text, MAX_NAME);
    lv_textarea_set_text(s_pw_text, text);
    lv_screen_load(s_pw_scr);
    if (s_focused) lv_obj_remove_state(s_focused, LV_STATE_FOCUSED);
    focus_clear_list();
}

static void on_sd_click(lv_event_t *ev)
{
    if (s_files_app[0]) snprintf(s_launch, sizeof(s_launch), "%s", s_files_app);
    else folder_message("Files is not installed", "Copy the Files app to /sd/apps/files/files.papp.");
}

static void on_count_click(lv_event_t *ev)
{
    if (s_folder[0]) {
        folder_leave();
        return;
    }
    s_name_move = false;
    name_open("Name of the new folder", "New folder", PW_NEW_FOLDER);
}

/* Moves the app's directory into a folder ("" = the main page) */
static void move_app(const char *path, const char *folder)
{
    char dir[160], dst[160];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    *slash = 0;
    const char *base = strrchr(dir, '/') + 1;
    if (folder[0]) snprintf(dst, sizeof(dst), APPS_DIR "/%s/%s", folder, base);
    else snprintf(dst, sizeof(dst), APPS_DIR "/%s", base);
    struct stat st;
    bool ok = stat(dst, &st) != 0 && rename(dir, dst) == 0;
    home_view(NULL);
    if (!ok) folder_message("Cannot move the app", "An app with the same directory name is already there.");
}

/* Only the empty folder goes: its apps return to the main page, and anything
 * else in it is kept (the folder stays) */
static void folder_delete(const char *name)
{
    char dir[64], src[128], dst[128];
    snprintf(dir, sizeof(dir), APPS_DIR "/%s", name);
    for (;;) {
        DIR *d = opendir(dir);
        if (!d) break;
        struct dirent *de;
        src[0] = 0;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.' || de->d_type != DT_DIR) continue;
            snprintf(src, sizeof(src), "%s/%s", dir, de->d_name);
            snprintf(dst, sizeof(dst), APPS_DIR "/%s", de->d_name);
            break;
        }
        closedir(d);                            /* never rename during readdir */
        if (!src[0]) break;
        struct stat st;
        if (stat(dst, &st) == 0 || rename(src, dst) != 0) {
            home_view(dir);
            folder_message("Cannot delete the folder", "An app inside has the same directory name as one on the main page.");
            return;
        }
    }
    bool ok = rmdir(dir) == 0;
    home_view(NULL);
    if (!ok) folder_message("Folder kept", "Its apps are back on the main page, but it still holds other files. Use the Files app to see them.");
}

static void folder_name_done(const char *text)
{
    char name[MAX_NAME + 1], path[64];
    while (*text == ' ') text++;
    snprintf(name, sizeof(name), "%s", text);
    for (size_t n = strlen(name); n && name[n - 1] == ' '; n--) name[n - 1] = 0;
    size_t n = strlen(name);
    if (!n || name[0] == '.' || name[n - 1] == '.' || strpbrk(name, "/\\:*?\"<>|")) {
        folder_message("Cannot use this name", "A name can't be empty, start or end with a dot, or use / \\ : * ? \" < > |");
        return;
    }
    snprintf(path, sizeof(path), APPS_DIR "/%s", name);
    bool same = s_pw_mode == PW_RENAME && strcasecmp(name, s_target_name) == 0;
    struct stat st;
    if (!same && stat(path, &st) == 0) {
        folder_message("Cannot use this name", "A folder or app directory already has this name.");
        return;
    }
    if (s_pw_mode == PW_RENAME) {
        if (rename(s_target, path) != 0) folder_message("Cannot rename", "The SD card refused the new name.");
        else home_view(path);
    } else if (mkdir(path, 0775) != 0) {
        folder_message("Cannot create the folder", "The SD card refused it.");
    } else if (s_name_move) {
        move_app(s_target, name);
    } else {
        home_view(path);
    }
}

static void tile_menu(const app_entry_t *e)
{
    snprintf(s_target, sizeof(s_target), "%s", e->path);
    snprintf(s_target_name, sizeof(s_target_name), "%s", e->name);
    if (e->is_folder) {
        menu_open(e->name, NULL, 4);
        menu_add("Open", ACT_OPEN);
        menu_add("Rename", ACT_RENAME);
        menu_add("Delete", ACT_DELETE);
    } else {
        menu_open(e->name, NULL, 3);
        menu_add("Move to folder", ACT_PICK);
        if (s_folder[0]) menu_add("Move to main page", ACT_OUT);
    }
    menu_add("Cancel", ACT_CLOSE);
}

static void on_menu_item(lv_event_t *ev)
{
    int act = (int)(intptr_t)lv_event_get_user_data(ev);
    menu_close();
    switch (act) {
    case ACT_CLOSE: break;
    case ACT_OPEN: folder_enter(s_target_name); break;
    case ACT_RENAME: name_open("New name of the folder", s_target_name, PW_RENAME); break;
    case ACT_DELETE:
        menu_open("Delete this folder?", "Its apps go back to the main page. No app is deleted.", 2);
        menu_add("Cancel", ACT_CLOSE);
        menu_add("Delete", ACT_DELETE_YES);
        break;
    case ACT_DELETE_YES: folder_delete(s_target_name); break;
    case ACT_PICK: {
        int n = 2;
        for (int i = 0; i < s_napps; i++) n += s_apps[i].is_folder;
        menu_open("Move to folder", NULL, n);
        for (int i = 0; i < s_napps; i++)
            if (s_apps[i].is_folder && strcmp(s_apps[i].name, s_folder) != 0)
                menu_add(s_apps[i].name, ACT_TO_FOLDER + i);
        menu_add(LV_SYMBOL_PLUS " New folder", ACT_NEW_MOVE);
        menu_add("Cancel", ACT_CLOSE);
        break;
    }
    case ACT_OUT: move_app(s_target, ""); break;
    case ACT_NEW_MOVE:
        s_name_move = true;
        name_open("Name of the new folder", "New folder", PW_NEW_FOLDER);
        break;
    default: move_app(s_target, s_apps[act - ACT_TO_FOLDER].name); break;
    }
}

/* ── Periodic updates: clock, SD, system bar ───────────────────────────── */

#define BAT_MV_EMPTY 3300               /* the reference's curve */
#define BAT_MV_FULL  3900
#define BAT_MV_MIN   2800               /* outside this: no cell fitted */
#define BAT_MV_MAX   4500

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
        snprintf(buf, sizeof(buf), "%d app%s installed", s_ninstalled, s_ninstalled == 1 ? "" : "s");
        lv_label_set_text(s_sd_apps, buf);
    }

    snprintf(buf, sizeof(buf), "%d MHz x2", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    lv_label_set_text(s_sys_cpu, buf);
    snprintf(buf, sizeof(buf), "SRAM %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    lv_label_set_text(s_sys_sram, buf);
    snprintf(buf, sizeof(buf), "PSRAM %.1f MB", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0);
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
    int bat = (bsp_info()->caps & BSP_CAP_BATTERY) ? bsp_battery_mv() : -1;
    if (bat >= BAT_MV_MIN && bat <= BAT_MV_MAX) {           /* a cell is fitted */
        int pct = (bat - BAT_MV_EMPTY) * 100 / (BAT_MV_FULL - BAT_MV_EMPTY);
        pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
        const char *sym = bat > BAT_MV_FULL ? LV_SYMBOL_CHARGE : pct > 75 ? LV_SYMBOL_BATTERY_FULL :
                          pct > 50 ? LV_SYMBOL_BATTERY_3 : pct > 25 ? LV_SYMBOL_BATTERY_2 :
                          pct > 8 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        snprintf(buf, sizeof(buf), LV_SYMBOL_USB " %d pad%s   %s %d%%", pads, pads == 1 ? "" : "s", sym, pct);
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_USB " %d pad%s", pads, pads == 1 ? "" : "s");
    }
    if (lx_net_state() == LX_NET_CONNECTED) {
        size_t n = strlen(buf);
        snprintf(buf + n, sizeof(buf) - n, "   " LV_SYMBOL_WIFI);
    }
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

/* ── Launcher update from the SD card ─────────────────────── */

#define CONFIRM_AFTER_US 10000000       /* a new version must survive this long */

static lv_obj_t *s_upd_box, *s_upd_text;
static volatile bool s_upd_go;

static void upd_close(void)
{
    lv_msgbox_close(s_upd_box);
    s_upd_box = s_upd_text = NULL;
    s_focused = NULL;
    home_focus_list();
}

static void on_upd_later(lv_event_t *ev) { upd_close(); }
static void on_upd_install(lv_event_t *ev) { s_upd_go = true; }

static void upd_progress(int percent)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "Installing... %d%%", percent);
    lv_label_set_text(s_upd_text, buf);
    lv_refr_now(s_disp);
}

static void upd_message(const char *title, const char *text)
{
    lv_obj_t *mb = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mb, title);
    lv_msgbox_add_text(mb, text);
    lv_obj_t *ok = lv_msgbox_add_footer_button(mb, "OK");
    lv_obj_add_event_cb(ok, on_crash_ok, LV_EVENT_CLICKED, mb);
    lv_obj_set_width(mb, 520);
    s_crash_box = mb;
    s_crash_ok = ok;
    focus_set(ok);
}

static void upd_offer(void)
{
    char ver[32], text[128];
    if (lx_update_rolled_back()) {
        upd_message(LV_SYMBOL_WARNING "  Update failed",
                    "The new launcher did not start, so the previous version was restored.");
        return;
    }
    if (!lx_update_available(ver, sizeof(ver))) return;
    snprintf(text, sizeof(text), "Install launcher %s from the SD card?\nThis version: %s",
             ver, LX_LAUNCHER_VERSION);
    s_upd_box = lv_msgbox_create(NULL);
    lv_msgbox_add_title(s_upd_box, LV_SYMBOL_DOWNLOAD "  Launcher update");
    s_upd_text = lv_msgbox_add_text(s_upd_box, text);
    lv_obj_t *yes = lv_msgbox_add_footer_button(s_upd_box, "Install");
    lv_obj_t *no = lv_msgbox_add_footer_button(s_upd_box, "Later");
    lv_obj_add_event_cb(yes, on_upd_install, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(no, on_upd_later, LV_EVENT_CLICKED, NULL);
    lv_obj_set_width(s_upd_box, 520);
    focus_clear_list();
    focus_add(yes);
    focus_add(no);
    focus_set(yes);
}

static void upd_install(void)
{
    s_upd_go = false;
    esp_err_t err = lx_update_install(upd_progress);
    if (err == ESP_OK) {
        lv_label_set_text(s_upd_text, "Done. Restarting...");
        lv_refr_now(s_disp);
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    char buf[80];
    snprintf(buf, sizeof(buf), "Install failed: %s", esp_err_to_name(err));
    lv_label_set_text(s_upd_text, buf);
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
        int dx = 0, dy = 0, ok = 0, back = 0, menu = 0;
        if (ev.type == PAPP_EV_BUTTON_DOWN) {
            switch (ev.code) {
            case 16: dy = -1; break;
            case 17: dy = 1; break;
            case 18: dx = -1; break;
            case 19: dx = 1; break;
            case 0: case 9: ok = 1; break;          /* A, START */
            case 1: back = 1; break;                /* B */
            case 2: case 3: menu = 1; break;        /* X, Y */
            default: break;
            }
        } else if (ev.type == PAPP_EV_KEY_DOWN) {
            if (lv_screen_active() == s_pw_scr) {   /* a USB keyboard types the password */
                if (ev.code == 0x28 || ev.code == 0x58) {
                    pw_accept();
                } else if (ev.code == 0x2A) {
                    lv_textarea_delete_char(s_pw_text);
                } else if (ev.code == 0x29) {
                    pw_close();
                } else if (ev.text >= 32 && ev.text < 127) {
                    lv_textarea_add_char(s_pw_text, ev.text);
                }
                continue;
            }
            switch (ev.code) {
            case 0x52: dy = -1; break;
            case 0x51: dy = 1; break;
            case 0x50: dx = -1; break;
            case 0x4F: dx = 1; break;
            case 0x28: case 0x58: case 0x2C: ok = 1; break;   /* Enter, keypad Enter, Space */
            case 0x29: case 0x2A: back = 1; break;            /* Esc, Backspace */
            case 0x2B: case 0x65: menu = 1; break;            /* Tab, Menu */
            default: break;
            }
        } else {
            continue;
        }
        if (s_crash_box) {
            if (ok || back) lv_obj_send_event(s_crash_ok, LV_EVENT_CLICKED, NULL);
            continue;
        }
        if (dx && s_focused == s_wifi_tz) {
            int n = lv_dropdown_get_option_count(s_wifi_tz);
            lv_dropdown_set_selected(s_wifi_tz, (lv_dropdown_get_selected(s_wifi_tz) + n + dx) % n);
            on_wifi_tz(NULL);
        } else if (dx && s_focused && lv_obj_check_type(s_focused, &lv_slider_class)) {
            lv_slider_set_value(s_focused, lv_slider_get_value(s_focused) + dx * 5, LV_ANIM_OFF);
            lv_obj_send_event(s_focused, LV_EVENT_VALUE_CHANGED, NULL);
            save_settings();
        } else if (dx || dy) {
            focus_move(dx, dy);
        } else if (ok && lv_screen_active() == s_pw_scr && s_pw_mode != PW_WIFI) {
            pw_accept();
        } else if (ok && s_focused) {
            lv_obj_send_event(s_focused, LV_EVENT_CLICKED, NULL);
        } else if (menu && s_focused && !s_menu && lv_obj_get_parent(s_focused) == s_grid) {
            tile_menu(lv_obj_get_user_data(s_focused));
        } else if (back && s_menu) {
            menu_close();
        } else if (back && lv_screen_active() == s_home && s_folder[0]) {
            folder_leave();
        } else if (back && lv_screen_active() == s_settings) {
            show_home();
        } else if (back && lv_screen_active() == s_wifi_scr) {
            show_settings();
        } else if (back && lv_screen_active() == s_pw_scr) {
            pw_close();
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
    bool was_running = false, confirmed = false, offered = false;
    for (;;) {
        if (!confirmed && esp_timer_get_time() > CONFIRM_AFTER_US) {
            lx_update_confirm();
            confirmed = true;
        }
        if (lx_app_state() != LX_APP_IDLE) {
            if (!was_running) {
                lx_net_pause(true);
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
            lx_net_pause(false);
            lx_input_flush();
            s_volume = lx_audio_master_volume_get();
            lv_slider_set_value(s_vol_slider, s_volume, LV_ANIM_OFF);
            apply_settings();
            if (lv_screen_active() != s_home) lv_screen_load(s_home);
            s_focused = NULL;
            refresh_home();
            lv_obj_invalidate(lv_screen_active());
        }
        if (confirmed && !offered && !s_crash_box) {
            offered = true;
            upd_offer();
        }
        if (s_map_step >= 0) map_tick();
        else poll_keys();
        wifi_tick();
        uint32_t wait = lv_timer_handler();
        if (s_upd_go) upd_install();
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
    ESP_LOGI(TAG, "home screen up, %d apps", s_ninstalled);
    return ESP_OK;
}
