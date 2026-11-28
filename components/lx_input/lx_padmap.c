/*
 * Controller button mappings as text on the SD card, one line per pad model:
 *
 *   054C:0268  A=B B=A X=Y Y=X  # Sony PLAYSTATION(R)3 Controller
 *
 * VID:PID, then <button the pad reports>=<button it should be> for every button
 * that is not itself ("-" = ignored), then the pad's name. Button names are the
 * PAPP_BTN_ ones: A B X Y L1 R1 L2 R2 SELECT START L3 R3 HOME MISC UP DOWN LEFT RIGHT.
 *
 * Since 0.38 a line may also carry (older launchers warn about these and go on):
 *   kind=gamepad|flightstick|throttle|wheel|arcade   what the device is (else guessed from its axes)
 *   <axis>=<stick>   an axis of the device (X Y Z RX RY RZ SLIDER DIAL WHEEL THROTTLE RUDDER STEERING ACCEL
 *                    BRAKE SLIDER2) to a stick or trigger of the standard pad (LX LY RX RY LT RT; "-RY" =
 *                    inverted, "-" = none). For devices read by their HID descriptor
 *   pic=<name>       the picture in /sd/config/pads (else the kind's)
 *   range=<degrees>  a force-feedback wheel's rotation, 40..900 (Settings -> Map controller; default 900)
 *   spring=<percent> its force strength, 0..100 (default 25): the centring spring outside apps, and the gain on
 *                    the forces apps ask for (25 = as asked, 50 = 1.5x, 75 = 2x, 0 = no force feedback)
 * for example
 *   044F:B10A  kind=flightstick SLIDER=-RY RZ=RX pic=flightstick  # a flight stick
 *
 * /sd/config/gamepads_mine.txt  written by Settings -> Map controller; wins
 * /sd/config/gamepads.txt       the shared database (SDcard/config/gamepads.txt
 *                               in the repository, updated by the Store)
 */
#include "lx_input.h"
#include "lx_input_priv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "lx_padmap";

#define MINE_FILE  LX_PADMAP_DIR "/gamepads_mine.txt"
#define DB_FILE    LX_PADMAP_DIR "/gamepads.txt"
#define LINE_MAX_  256
#define FILE_MAX   (64 * 1024)

static const char *const k_names[LX_INPUT_MAP_BITS] = {
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "SELECT", "START",
    "L3", "R3", "HOME", "MISC", "BIT14", "BIT15", "UP", "DOWN", "LEFT", "RIGHT",
};

static int name_bit(const char *s, size_t n)
{
    for (int i = 0; i < LX_INPUT_MAP_BITS; i++)
        if (strlen(k_names[i]) == n && strncasecmp(k_names[i], s, n) == 0) return i;
    return -1;
}

/* HID_AX_ order */
static const char *const k_axes[HID_AX_COUNT] = {
    "X", "Y", "Z", "RX", "RY", "RZ", "SLIDER", "DIAL", "WHEEL", "THROTTLE", "RUDDER", "STEERING", "ACCEL",
    "BRAKE", "SLIDER2",
};
static const char *const k_sticks[] = { "LX", "LY", "RX", "RY", "LT", "RT" };     /* LX_PADAX_ */
static const char *const k_kinds[] = { "gamepad", "flightstick", "throttle", "wheel", NULL, NULL, "arcade" };

static int name_in(const char *const *t, int n, const char *s, size_t len)
{
    for (int i = 0; i < n; i++)
        if (t[i] && strlen(t[i]) == len && strncasecmp(t[i], s, len) == 0) return i;
    return -1;
}

static bool is_key(const char *s, size_t n, const char *key)
{
    return strlen(key) == n && strncasecmp(key, s, n) == 0;
}

/* A key of the 0.38 additions (kind, pic, an axis)? */
static bool ext_key(const char *s, size_t n)
{
    return is_key(s, n, "kind") || is_key(s, n, "pic") || is_key(s, n, "range") || is_key(s, n, "spring") ||
           name_in(k_axes, HID_AX_COUNT, s, n) >= 0;
}

/* One token key=value of the additions into *e */
static void ext_token(lx_padmap_ext_t *e, const char *k, size_t kn, const char *v, size_t vn)
{
    int ax;
    if (is_key(k, kn, "kind")) {
        int kind = name_in(k_kinds, (int)(sizeof(k_kinds) / sizeof(k_kinds[0])), v, vn);
        if (kind >= 0) e->kind = (int8_t)kind;
    } else if (is_key(k, kn, "pic")) {
        snprintf(e->pic, sizeof(e->pic), "%.*s", (int)vn, v);
    } else if (is_key(k, kn, "range") || is_key(k, kn, "spring")) {
        int n = atoi(v);
        if (is_key(k, kn, "range")) e->range = (uint16_t)(n < 40 ? 40 : n > 900 ? 900 : n);
        else { e->has_spring = true; e->spring = (uint8_t)(n < 0 ? 0 : n > 100 ? 100 : n); }
    } else if ((ax = name_in(k_axes, HID_AX_COUNT, k, kn)) >= 0 && e->naxis < LX_PADMAP_AXES) {
        bool inv = vn > 1 && *v == '-';
        int to = vn == 1 && *v == '-' ? 0xFF : name_in(k_sticks, 6, v + inv, vn - inv);
        if (to < 0) return;
        e->axis[e->naxis].role = (uint8_t)ax;
        e->axis[e->naxis].to = (uint8_t)to;
        e->axis[e->naxis++].inv = inv;
    }
}

/* Line for vid:pid? Then fills map (identity, then the line's changes) and *ext (when not NULL). */
static bool parse_line_ext(const char *line, uint16_t vid, uint16_t pid, uint8_t *map, lx_padmap_ext_t *ext)
{
    while (*line == ' ' || *line == '\t') line++;
    unsigned v, p;
    int used = 0;
    if (sscanf(line, "%4x:%4x%n", &v, &p, &used) != 2 || v != vid || p != pid) return false;
    for (int i = 0; i < LX_INPUT_MAP_BITS; i++) map[i] = (uint8_t)i;
    const char *s = line + used;
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#' || *s == '\r' || *s == '\n') break;
        const char *eq = s;
        while (*eq && *eq != '=' && !isspace((unsigned char)*eq)) eq++;
        const char *val = eq + 1, *end = val;
        while (*end && !isspace((unsigned char)*end) && *end != '#') end++;
        if (*eq == '=') {
            int from = name_bit(s, eq - s);
            int to = (end - val == 1 && *val == '-') ? 0xFF : name_bit(val, end - val);
            if (from >= 0 && to != -1) map[from] = (uint8_t)to;
            else if (ext_key(s, eq - s)) { if (ext) ext_token(ext, s, eq - s, val, end - val); }
            else ESP_LOGW(TAG, "%04X:%04X: '%.*s' not understood", vid, pid, (int)(end - s), s);
        }
        s = end;
    }
    return true;
}

static bool parse_line(const char *line, uint16_t vid, uint16_t pid, uint8_t *map)
{
    return parse_line_ext(line, vid, pid, map, NULL);
}

/* The additions' tokens of a line, as text (kept when Map controller rewrites the line) */
static void ext_text(const char *line, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = 0;
    const char *s = line;
    while (*s && *s != ' ' && *s != '\t') s++;          /* VID:PID */
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#' || *s == '\r' || *s == '\n') break;
        const char *eq = s, *end;
        while (*eq && *eq != '=' && !isspace((unsigned char)*eq)) eq++;
        for (end = eq; *end && !isspace((unsigned char)*end) && *end != '#'; end++) {}
        if (*eq == '=' && ext_key(s, eq - s) && n + (end - s) + 2 < cap)
            n += snprintf(out + n, cap - n, " %.*s", (int)(end - s), s);
        s = end;
    }
}

static bool load_from(const char *path, uint16_t vid, uint16_t pid, uint8_t *map)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[LINE_MAX_];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) found = parse_line(line, vid, pid, map);
    fclose(f);
    return found;
}

static bool ext_from(const char *path, uint16_t vid, uint16_t pid, lx_padmap_ext_t *ext)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[LINE_MAX_];
    uint8_t map[LX_INPUT_MAP_BITS];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) found = parse_line_ext(line, vid, pid, map, ext);
    fclose(f);
    return found;
}

bool lx_padmap_ext(uint16_t vid, uint16_t pid, lx_padmap_ext_t *ext)
{
    memset(ext, 0, sizeof(*ext));
    ext->kind = -1;
    if (ext_from(MINE_FILE, vid, pid, ext)) return true;
    memset(ext, 0, sizeof(*ext));
    ext->kind = -1;
    return ext_from(DB_FILE, vid, pid, ext);
}

int lx_padmap_load(uint16_t vid, uint16_t pid, uint8_t *map)
{
    if (load_from(MINE_FILE, vid, pid, map)) return LX_PADMAP_MINE;
    if (load_from(DB_FILE, vid, pid, map)) return LX_PADMAP_DB;
    return LX_PADMAP_NONE;
}

/* The database's line for vid:pid: its additions as text (when your own line doesn't exist yet) */
static void db_ext_text(uint16_t vid, uint16_t pid, char *out, size_t cap)
{
    out[0] = 0;
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return;
    char line[LINE_MAX_];
    uint8_t scratch[LX_INPUT_MAP_BITS];
    while (fgets(line, sizeof(line), f))
        if (parse_line(line, vid, pid, scratch)) { ext_text(line, out, cap); break; }
    fclose(f);
}

/* Drops the range= / spring= tokens of additions text */
static void strip_wheel(char *extra)
{
    char out[128];
    size_t n = 0;
    out[0] = 0;
    for (char *t = strtok(extra, " "); t; t = strtok(NULL, " "))
        if (strncasecmp(t, "range=", 6) && strncasecmp(t, "spring=", 7) && n + strlen(t) + 2 < sizeof(out))
            n += snprintf(out + n, sizeof(out) - n, " %s", t);
    strcpy(extra, out);
}

/* wheel: " range=.. spring=.." to set, NULL = keep what the line has */
static esp_err_t save_line(uint16_t vid, uint16_t pid, const char *name, const uint8_t *map, const char *wheel)
{
    char *old = heap_caps_calloc(1, FILE_MAX, MALLOC_CAP_SPIRAM);
    if (!old) return ESP_ERR_NO_MEM;
    size_t n = 0;
    FILE *f = fopen(MINE_FILE, "r");
    if (f) {
        n = fread(old, 1, FILE_MAX - 1, f);
        fclose(f);
    }
    mkdir(LX_PADMAP_DIR, 0775);
    f = fopen(MINE_FILE ".tmp", "w");
    if (!f) {
        free(old);
        return ESP_FAIL;
    }
    if (!n) fputs("# Your controller mappings (Settings -> Map controller). To share one, add its line to\n"
                  "# SDcard/config/gamepads.txt in https://github.com/giltal/ESP32_P4_Launcher\n", f);
    uint8_t scratch[LX_INPUT_MAP_BITS];
    char extra[128] = "";
    bool mine = false;
    for (char *line = old; line < old + n;) {                /* keep every other pad's line */
        char *next = strchr(line, '\n');
        next = next ? next + 1 : old + n;
        if (!parse_line(line, vid, pid, scratch)) fwrite(line, 1, next - line, f);
        else { ext_text(line, extra, sizeof(extra)); mine = true; }   /* this pad's kind, axes, picture stay */
        line = next;
    }
    /* your first line for it: the database's kind, axes and picture come along (yours wins whole) */
    if (!mine && (map || wheel)) db_ext_text(vid, pid, extra, sizeof(extra));
    if (wheel) {
        strip_wheel(extra);
        strncat(extra, wheel, sizeof(extra) - strlen(extra) - 1);
    }
    if (map || extra[0]) {
        fprintf(f, "%04X:%04X ", vid, pid);
        for (int i = 0; map && i < LX_INPUT_MAP_BITS; i++)
            if (map[i] != i) fprintf(f, " %s=%s", k_names[i], map[i] < LX_INPUT_MAP_BITS ? k_names[map[i]] : "-");
        fprintf(f, "%s  # %s\n", extra, name && name[0] ? name : "unnamed controller");
    }
    free(old);
    bool ok = fclose(f) == 0;
    if (ok) {
        remove(MINE_FILE);                  /* FAT rename doesn't replace */
        ok = rename(MINE_FILE ".tmp", MINE_FILE) == 0;
    }
    if (!ok) ESP_LOGW(TAG, "could not write %s", MINE_FILE);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t lx_padmap_save(uint16_t vid, uint16_t pid, const char *name, const uint8_t *map)
{
    return save_line(vid, pid, name, map, NULL);
}

esp_err_t lx_padmap_save_wheel(uint16_t vid, uint16_t pid, const char *name, uint16_t range, int spring_pct)
{
    uint8_t map[LX_INPUT_MAP_BITS];
    bool has = lx_padmap_load(vid, pid, map) != LX_PADMAP_NONE;     /* the buttons as they are now */
    char w[40];
    snprintf(w, sizeof(w), " range=%u spring=%d", (unsigned)range, spring_pct);
    return save_line(vid, pid, name, has ? map : NULL, w);
}
