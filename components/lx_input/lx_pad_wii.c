/*
 * Wii Remote (Nintendo RVL-CNT-01, -TR) and what plugs into it (Nunchuk, Classic Controller / Pro), and
 * the Wii U Pro Controller (RVL-CNT-01-UC, which talks like a remote with a built-in extension), over
 * Bluetooth Classic HID (lx_input_hidpad.c, transport lx_bt_pads.c).
 *
 * Protocol (wiibrew.org "Wiimote", Linux hid-wiimote): output reports on the interrupt channel, byte 1 bit
 * 0 of every one is the rumble motor. 0x11 LEDs, 0x12 data mode, 0x15 status request, 0x16 write memory,
 * 0x17 read memory. Input: 0x20 status (battery, extension plugged), 0x21 memory read, 0x22 write
 * acknowledge, 0x30.. data reports with the core buttons first.
 *
 * Setup: status request; on its answer the extension, if any, is set up unencrypted (0x55 to 0xA400F0,
 * 0x00 to 0xA400FB), its id read at 0xA400FA, then the data mode: 0x31 (buttons + accelerometer) for the
 * remote alone, 0x35 (+ 16 extension bytes) with an extension. Every unsolicited status report (an
 * extension plugged or pulled) starts this again.
 *
 * Standard-pad views:
 *   remote alone, held sideways (d-pad on the left, NES style): d-pad turned, 1 = B, 2 = A, A = X,
 *   B (trigger) = Y, - = Select, + = Start, HOME = HOME
 *   remote + Nunchuk, upright: Nunchuk stick = left stick, d-pad, A = A, B = B, C = L1, Z = L2, 1 = X,
 *   2 = Y, -/+ = Select/Start
 *   Classic Controller / Pro, Wii U Pro: a normal pad in Nintendo positions (B south = PAPP A ...)
 */
#include "lx_input.h"
#include "lx_input_priv.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "lx_wii";

enum { EXT_NONE, EXT_UNKNOWN, EXT_NUNCHUK, EXT_CLASSIC, EXT_PRO };
enum { ST_IDLE, ST_INIT1, ST_INIT2, ST_READ_ID };

#define REG_EXT_INIT1  0xA400F0
#define REG_EXT_INIT2  0xA400FB
#define REG_EXT_ID     0xA400FA

static void send(lx_wii_t *w, uint8_t *r, size_t n)
{
    r[1] = (uint8_t)((r[1] & ~1u) | (w->rumble ? 1 : 0));
    if (w->send) w->send(w->ctx, r, n);
}

static void write_reg(lx_wii_t *w, uint32_t addr, uint8_t v)
{
    uint8_t r[22] = { 0x16, 0x04, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, 1, v };
    send(w, r, sizeof(r));
}

static void read_reg(lx_wii_t *w, uint32_t addr, uint16_t size)
{
    uint8_t r[7] = { 0x17, 0x04, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
                     (uint8_t)(size >> 8), (uint8_t)size };
    send(w, r, sizeof(r));
}

static void set_mode(lx_wii_t *w)
{
    uint8_t r[3] = { 0x12, 0x00, (uint8_t)(w->ext >= EXT_NUNCHUK ? 0x35 : 0x31) };
    send(w, r, sizeof(r));
}

static void status_request(lx_wii_t *w)
{
    uint8_t r[2] = { 0x15, 0x00 };
    send(w, r, sizeof(r));
}

void lx_wii_start(lx_wii_t *w, void (*fn)(void *ctx, const uint8_t *data, size_t len), void *ctx, bool pro)
{
    memset(w, 0, sizeof(*w));
    w->send = fn;
    w->ctx = ctx;
    w->pro = pro;
    status_request(w);
}

void lx_wii_out(lx_wii_t *w, const lx_pad_out_state_t *o)
{
    w->rumble = o->strong > 0x2000 || o->weak > 0x4000;  /* one motor, on or off */
    w->leds = (uint8_t)(0x10 << (o->player & 3));
    uint8_t r[2] = { 0x11, w->leds };
    send(w, r, sizeof(r));
}

int lx_wii_kind(const lx_wii_t *w)
{
    if (w->ext == EXT_NUNCHUK) return PAPP_PAD_WIIMOTE_NUNCHUK;
    if (w->ext == EXT_CLASSIC || w->ext == EXT_PRO || w->pro) return PAPP_PAD_GAMEPAD;
    return PAPP_PAD_WIIMOTE;
}

/* Core buttons as they are on the remote */
enum {
    W_LEFT = 1 << 0, W_RIGHT = 1 << 1, W_DOWN = 1 << 2, W_UP = 1 << 3, W_PLUS = 1 << 4,
    W_TWO = 1 << 8, W_ONE = 1 << 9, W_B = 1 << 10, W_A = 1 << 11, W_MINUS = 1 << 12, W_HOME = 1 << 15,
};

static uint32_t remote_sideways(uint16_t c)
{
    uint32_t b = 0;
    if (c & W_UP) b |= PAPP_BTN_LEFT;           /* turned a quarter to the left */
    if (c & W_DOWN) b |= PAPP_BTN_RIGHT;
    if (c & W_LEFT) b |= PAPP_BTN_DOWN;
    if (c & W_RIGHT) b |= PAPP_BTN_UP;
    if (c & W_ONE) b |= PAPP_BTN_B;
    if (c & W_TWO) b |= PAPP_BTN_A;
    if (c & W_A) b |= PAPP_BTN_X;
    if (c & W_B) b |= PAPP_BTN_Y;
    if (c & W_MINUS) b |= PAPP_BTN_SELECT;
    if (c & W_PLUS) b |= PAPP_BTN_START;
    if (c & W_HOME) b |= PAPP_BTN_HOME;
    return b;
}

static uint32_t remote_upright(uint16_t c)
{
    uint32_t b = 0;
    if (c & W_UP) b |= PAPP_BTN_UP;
    if (c & W_DOWN) b |= PAPP_BTN_DOWN;
    if (c & W_LEFT) b |= PAPP_BTN_LEFT;
    if (c & W_RIGHT) b |= PAPP_BTN_RIGHT;
    if (c & W_A) b |= PAPP_BTN_A;
    if (c & W_B) b |= PAPP_BTN_B;
    if (c & W_ONE) b |= PAPP_BTN_X;
    if (c & W_TWO) b |= PAPP_BTN_Y;
    if (c & W_MINUS) b |= PAPP_BTN_SELECT;
    if (c & W_PLUS) b |= PAPP_BTN_START;
    if (c & W_HOME) b |= PAPP_BTN_HOME;
    return b;
}

/* Classic Controller (Pro) and Wii U Pro buttons: two bytes, active low, the same bits in both */
static uint32_t classic_buttons(uint8_t b4, uint8_t b5)
{
    uint8_t x = (uint8_t)~b4, y = (uint8_t)~b5;
    uint32_t b = 0;
    if (x & 0x80) b |= PAPP_BTN_RIGHT;
    if (x & 0x40) b |= PAPP_BTN_DOWN;
    if (x & 0x20) b |= PAPP_BTN_L1;
    if (x & 0x10) b |= PAPP_BTN_SELECT;
    if (x & 0x08) b |= PAPP_BTN_HOME;
    if (x & 0x04) b |= PAPP_BTN_START;
    if (x & 0x02) b |= PAPP_BTN_R1;
    if (y & 0x80) b |= PAPP_BTN_L2;             /* ZL */
    if (y & 0x40) b |= PAPP_BTN_A;              /* B, south */
    if (y & 0x20) b |= PAPP_BTN_X;              /* Y, west */
    if (y & 0x10) b |= PAPP_BTN_B;              /* A, east */
    if (y & 0x08) b |= PAPP_BTN_Y;              /* X, north */
    if (y & 0x04) b |= PAPP_BTN_R2;             /* ZR */
    if (y & 0x02) b |= PAPP_BTN_LEFT;
    if (y & 0x01) b |= PAPP_BTN_UP;
    return b;
}

static int16_t scale(int v, int centre, int half, bool invert)
{
    int s = (v - centre) * 32767 / half;
    if (invert) s = -s;
    return (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

static void ext_state(lx_wii_t *w, const uint8_t *e, size_t n, uint16_t core, lx_pad_state_t *s)
{
    switch (w->ext) {
    case EXT_NUNCHUK:
        if (n < 6) break;
        s->b = remote_upright(core);
        if (!(e[5] & 0x02)) s->b |= PAPP_BTN_L1;    /* C */
        if (!(e[5] & 0x01)) { s->b |= PAPP_BTN_L2; s->lt = 65535; }   /* Z */
        s->lx = scale(e[0], 128, 100, false);
        s->ly = scale(e[1], 128, 100, true);
        break;
    case EXT_CLASSIC:
        if (n < 6) break;
        s->b = classic_buttons(e[4], e[5]) | (core & W_HOME ? PAPP_BTN_HOME : 0);
        s->lx = scale(e[0] & 0x3F, 32, 28, false);
        s->ly = scale(e[1] & 0x3F, 32, 28, true);
        s->rx = scale((e[0] >> 6 & 3) << 3 | (e[1] >> 6 & 3) << 1 | e[2] >> 7, 16, 14, false);
        s->ry = scale(e[2] & 0x1F, 16, 14, true);
        s->lt = (uint16_t)(((e[2] >> 5 & 3) << 3 | e[3] >> 5) * 2114);    /* 0..31 */
        s->rt = (uint16_t)((e[3] & 0x1F) * 2114);
        break;
    case EXT_PRO:
        if (n < 11) break;
        s->b = classic_buttons(e[8], e[9]) | (core & W_HOME ? PAPP_BTN_HOME : 0);
        if (!(e[10] & 0x02)) s->b |= PAPP_BTN_L3;
        if (!(e[10] & 0x01)) s->b |= PAPP_BTN_R3;
        s->lx = scale(e[0] | (e[1] & 0x0F) << 8, 2048, 1200, false);
        s->rx = scale(e[2] | (e[3] & 0x0F) << 8, 2048, 1200, false);
        s->ly = scale(e[4] | (e[5] & 0x0F) << 8, 2048, 1200, true);
        s->ry = scale(e[6] | (e[7] & 0x0F) << 8, 2048, 1200, true);
        s->lt = s->b & PAPP_BTN_L2 ? 65535 : 0;
        s->rt = s->b & PAPP_BTN_R2 ? 65535 : 0;
        break;
    default:
        s->b = remote_sideways(core);
        break;
    }
}

static int ext_type(const uint8_t *id)
{
    if (id[2] == 0xA4 && id[3] == 0x20) {
        if (id[4] == 0x00 && id[5] == 0x00) return EXT_NUNCHUK;
        if (id[4] == 0x01 && id[5] == 0x01) return EXT_CLASSIC;
        if (id[4] == 0x01 && id[5] == 0x20) return EXT_PRO;
    }
    return EXT_UNKNOWN;
}

bool lx_wii_report(lx_wii_t *w, const uint8_t *p, size_t len, lx_pad_state_t *s, papp_motion_t *m, bool *has_m)
{
    static const char *const ext_name[] = { "none", "unknown", "Nunchuk", "Classic Controller", "Wii U Pro" };
    *has_m = false;
    if (len < 3) return false;
    uint8_t id = p[0];
    uint16_t core = (uint16_t)((p[1] | p[2] << 8) & 0x9F1F);

    if (id == 0x20 && len >= 7) {               /* status: battery, extension */
        bool ext = (p[3] & 0x02) || w->pro;     /* a Wii U Pro: its "extension" is built in */
        w->battery = p[6];
        if (ext && w->st == ST_IDLE) {          /* (again after every status: it may be another one) */
            w->st = ST_INIT1;
            write_reg(w, REG_EXT_INIT1, 0x55);
        } else if (!ext) {
            if (w->ext != EXT_NONE) {
                ESP_LOGI(TAG, "extension removed");
                w->ext = EXT_NONE;
                w->kind_changed = true;
            }
            set_mode(w);
        }
        ESP_LOGI(TAG, "status: battery %d %%, extension %s", w->battery * 100 / 200, ext ? "plugged" : "none");
    } else if (id == 0x22 && len >= 5) {        /* write acknowledged */
        if (p[4]) ESP_LOGW(TAG, "write refused: %d", p[4]);
        if (w->st == ST_INIT1) {
            w->st = ST_INIT2;
            write_reg(w, REG_EXT_INIT2, 0x00);
        } else if (w->st == ST_INIT2) {
            w->st = ST_READ_ID;
            read_reg(w, REG_EXT_ID, 6);
        }
    } else if (id == 0x21 && len >= 12) {       /* memory read */
        uint16_t addr = (uint16_t)(p[4] << 8 | p[5]);
        if (w->st == ST_READ_ID && addr == (REG_EXT_ID & 0xFFFF) && !(p[3] & 0x0F)) {
            int t = ext_type(p + 6);
            if (w->pro && t == EXT_UNKNOWN) t = EXT_PRO;
            if (t != w->ext) w->kind_changed = true;
            w->ext = (uint8_t)t;
            ESP_LOGI(TAG, "extension: %s (%02X%02X%02X%02X%02X%02X)", ext_name[t], p[6], p[7], p[8], p[9], p[10], p[11]);
        } else if (w->st == ST_READ_ID) {
            w->ext = EXT_UNKNOWN;
        }
        w->st = ST_IDLE;
        set_mode(w);
    }

    /* data reports: the core buttons in every one, then accelerometer and / or extension bytes */
    const uint8_t *acc = NULL, *ext = NULL;
    size_t next = 0;
    switch (id) {
    case 0x30: break;
    case 0x31: acc = p + 3; break;
    case 0x32: ext = p + 3; next = 8; break;
    case 0x33: acc = p + 3; break;
    case 0x34: ext = p + 3; next = 19; break;
    case 0x35: acc = p + 3; ext = p + 6; next = 16; break;
    case 0x37: acc = p + 3; ext = p + 16; next = 6; break;
    case 0x20: case 0x21: case 0x22: break;
    default: return false;
    }
    if (acc && len >= 6) {
        /* 10 bits: the top 8 here, the low bits in the button bytes; 0 g ~ 512, about 100 per g */
        int x = acc[0] << 2 | (p[1] >> 5 & 3), y = acc[1] << 2 | (p[2] >> 4 & 2), z = acc[2] << 2 | (p[2] >> 5 & 2);
        memset(m, 0, sizeof(*m));
        m->valid = PAPP_MOTION_ACCEL;
        m->accel[0] = (int16_t)((x - 512) * 41);
        m->accel[1] = (int16_t)((y - 512) * 41);
        m->accel[2] = (int16_t)((z - 512) * 41);
        m->ir_x = m->ir_y = 0xFFFF;
        m->time_ms = (uint32_t)(esp_timer_get_time() / 1000);
        *has_m = true;
    }
    memset(s, 0, sizeof(*s));
    if (ext && len >= (size_t)(ext - p) + next) ext_state(w, ext, next, core, s);
    else if (w->ext == EXT_NUNCHUK) s->b = remote_upright(core);
    else if (w->ext == EXT_NONE || w->ext == EXT_UNKNOWN) s->b = remote_sideways(core);
    else s->b = core & W_HOME ? PAPP_BTN_HOME : 0;
    return true;
}
