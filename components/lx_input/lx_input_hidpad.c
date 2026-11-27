/*
 * HID gamepads that arrive by another transport than USB (Bluetooth Classic, lx_bt_pads.c): the
 * transport hands over the report descriptor once and then every input report.
 *
 * Sony and Nintendo pads send their own formats over Bluetooth and are read by model:
 *   DualShock 4 / DualSense: report 0x01 (the short report both send until asked for more, same
 *   layout as the DS4's USB report), DS4 0x11 (full, 2 bytes later), DualSense 0x31 (full, the
 *   USB layout 1 byte later);
 *   Switch Pro (and pads that pose as one, 8BitDo in Switch mode): 0x3F (simple HID mode, what it
 *   sends after connecting) and 0x30 (full mode);
 *   Wii Remote (+ Nunchuk, Classic Controller), Wii U Pro: lx_pad_wii.c.
 * Others: the report descriptor (lx_hid_parse), as for generic USB pads; a saved mapping fixes
 * the button order.
 */
#include "lx_input.h"
#include "lx_input_priv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lx_common.h"

static const char *TAG = "lx_hidpad";

enum { K_HID, K_SONY, K_SWITCH, K_NONE, K_WII };

struct lx_hidpad {
    int      player;
    int      kind;
    uint16_t vid, pid;
    bool     has_layout;
    hid_layout_t layout;
    hid_state_t raw;            /* K_HID: the controls, kept across reports */
    int      pkind;             /* PAPP_PAD_* */
    lx_padmap_ext_t ext;        /* gamepads.txt: axis remaps */
    uint32_t ext_gen;
    /* outputs (lx_input_hidpad_set_sender) */
    lx_hidpad_send_fn send;
    void    *send_ctx;
    uint8_t  seq;               /* DualSense / Switch packet counter */
    bool     out_ready;         /* Switch: vibration enabled, LEDs set */
    lx_wii_t wii;
    uint8_t  last_id;           /* report id of the last report seen, for the log */
    /* report timing, for lx_input_hidpad_stats (DevLink "bt status"): a link that stalls shows */
    int64_t  t_last, t_window;
    uint32_t n, gap_max_us, stalls, stalls_use;
    int64_t  t_change;                      /* last input change */
    lx_pad_state_t prev;
    bool     have_prev;
};

static int16_t stick16(const uint8_t *p, bool invert)
{
    int v = (int)(p[0] | p[1] << 8) - 32768;
    if (invert) v = -v - 1;
    return (int16_t)v;
}

static int16_t stick12(int v, bool invert)
{
    v = (v - 2048) * 16;
    if (invert) v = -v;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* Nintendo's positions: B south, A east, Y west, X north = PAPP A, B, X, Y */
static bool decode_switch(const uint8_t *p, size_t len, lx_pad_state_t *s)
{
    memset(s, 0, sizeof(*s));
    if (p[0] == 0x3F && len >= 12) {
        uint32_t b = lx_pad_hat(p[3]);
        if (p[1] & 0x01) b |= PAPP_BTN_A;
        if (p[1] & 0x02) b |= PAPP_BTN_B;
        if (p[1] & 0x04) b |= PAPP_BTN_X;
        if (p[1] & 0x08) b |= PAPP_BTN_Y;
        if (p[1] & 0x10) b |= PAPP_BTN_L1;
        if (p[1] & 0x20) b |= PAPP_BTN_R1;
        if (p[1] & 0x40) b |= PAPP_BTN_L2;
        if (p[1] & 0x80) b |= PAPP_BTN_R2;
        if (p[2] & 0x01) b |= PAPP_BTN_SELECT;
        if (p[2] & 0x02) b |= PAPP_BTN_START;
        if (p[2] & 0x04) b |= PAPP_BTN_L3;
        if (p[2] & 0x08) b |= PAPP_BTN_R3;
        if (p[2] & 0x10) b |= PAPP_BTN_HOME;
        if (p[2] & 0x20) b |= PAPP_BTN_MISC;
        s->b = b;
        s->lx = stick16(p + 4, false);  s->ly = stick16(p + 6, false);
        s->rx = stick16(p + 8, false);  s->ry = stick16(p + 10, false);
        s->lt = p[1] & 0x40 ? 65535 : 0;
        s->rt = p[1] & 0x80 ? 65535 : 0;
        return true;
    }
    if (p[0] == 0x30 && len >= 12) {
        /* [30][timer][battery][right][shared][left][LS x3][RS x3]; sticks 12 bit, up = more */
        uint8_t r = p[3], m = p[4], l = p[5];
        uint32_t b = 0;
        if (r & 0x01) b |= PAPP_BTN_X;          /* Y */
        if (r & 0x02) b |= PAPP_BTN_Y;          /* X */
        if (r & 0x04) b |= PAPP_BTN_A;          /* B */
        if (r & 0x08) b |= PAPP_BTN_B;          /* A */
        if (r & 0x40) b |= PAPP_BTN_R1;
        if (r & 0x80) b |= PAPP_BTN_R2;
        if (m & 0x01) b |= PAPP_BTN_SELECT;
        if (m & 0x02) b |= PAPP_BTN_START;
        if (m & 0x04) b |= PAPP_BTN_R3;
        if (m & 0x08) b |= PAPP_BTN_L3;
        if (m & 0x10) b |= PAPP_BTN_HOME;
        if (m & 0x20) b |= PAPP_BTN_MISC;
        if (l & 0x01) b |= PAPP_BTN_DOWN;
        if (l & 0x02) b |= PAPP_BTN_UP;
        if (l & 0x04) b |= PAPP_BTN_RIGHT;
        if (l & 0x08) b |= PAPP_BTN_LEFT;
        if (l & 0x40) b |= PAPP_BTN_L1;
        if (l & 0x80) b |= PAPP_BTN_L2;
        s->b = b;
        s->lx = stick12(p[6] | (p[7] & 0x0F) << 8, false);
        s->ly = stick12(p[7] >> 4 | p[8] << 4, true);
        s->rx = stick12(p[9] | (p[10] & 0x0F) << 8, false);
        s->ry = stick12(p[10] >> 4 | p[11] << 4, true);
        s->lt = l & 0x80 ? 65535 : 0;
        s->rt = r & 0x80 ? 65535 : 0;
        return true;
    }
    return false;
}

static bool decode_sony(const uint8_t *p, size_t len, lx_pad_state_t *s)
{
    memset(s, 0, sizeof(*s));
    if (p[0] == 0x01 && len >= 10) { lx_pad_ps4(p, s); return true; }
    if (p[0] == 0x11 && len >= 12) { lx_pad_ps4(p + 2, s); return true; }     /* DS4 full */
    if (p[0] == 0x31 && len >= 12) { lx_pad_ps5(p + 1, s); return true; }     /* DualSense full */
    return false;
}

/* ── Outputs: rumble and lights over the transport (Bluetooth: the interrupt channel) ── */

static bool is_dualsense(const lx_hidpad_t *h) { return h->pid == 0x0CE6 || h->pid == 0x0DF2; }

/* Bluetooth reports of Sony pads end with a CRC-32 over 0xA2 (DATA | OUTPUT) and the report */
static void sony_crc(uint8_t *r, size_t len)
{
    static const uint8_t seed = 0xA2;
    uint32_t crc = lx_crc32(lx_crc32(0, &seed, 1), r, len - 4);
    r[len - 4] = (uint8_t)crc; r[len - 3] = (uint8_t)(crc >> 8);
    r[len - 2] = (uint8_t)(crc >> 16); r[len - 1] = (uint8_t)(crc >> 24);
}

/* DualShock 4: report 0x11; DualSense: 0x31 (layouts as in Linux's hid-playstation). Either one also
 * switches the pad to its full reports (with motion data) */
static void sony_out(lx_hidpad_t *h, const lx_pad_out_state_t *s)
{
    uint8_t r[78] = { 0 };
    if (is_dualsense(h)) {
        r[0] = 0x31;
        r[1] = (uint8_t)((h->seq++ & 0x0F) << 4);
        r[2] = 0x10;
        r[3] = 0x03;                                /* valid 0: rumble (compatible), haptics select */
        r[4] = 0x04 | 0x10;                         /* valid 1: light bar, player LEDs */
        r[5] = (uint8_t)(s->weak >> 8);
        r[6] = (uint8_t)(s->strong >> 8);
        r[41] = 0x02;                               /* valid 2: light bar setup */
        r[44] = 0x02;                               /* end the blue start-up glow */
        r[46] = lx_pad_ps5_leds(s->player);
        r[47] = s->r; r[48] = s->g; r[49] = s->b;
    } else {
        r[0] = 0x11;
        r[1] = 0xC4;                                /* HID, CRC, report every 4 ms */
        r[3] = 0x07;                                /* rumble, light bar, flash */
        r[6] = (uint8_t)(s->weak >> 8);
        r[7] = (uint8_t)(s->strong >> 8);
        r[8] = s->r; r[9] = s->g; r[10] = s->b;
    }
    sony_crc(r, sizeof(r));
    h->send(h->send_ctx, r, sizeof(r));
}

/* Switch Pro HD rumble, one band: frequency (Hz) and amplitude 0..1 as the 2 bytes of the low or high
 * band (encoding from dekuNukem's Nintendo_Switch_Reverse_Engineering) */
static uint8_t sw_amp(float a)
{
    if (a <= 0) return 0;
    if (a > 1) a = 1;
    float e = a > 0.23f ? log2f(a * 8.7f) * 32.0f : a > 0.12f ? log2f(a * 17.0f) * 16.0f
                                                                : ((log2f(a) * 32.0f) - 96.0f) / (4.0f - 2.0f * a);
    return e < 0 ? 0 : e > 100 ? 100 : (uint8_t)lroundf(e);
}

static void sw_rumble(uint8_t out[4], float hi_amp, float lo_amp)
{
    const uint16_t hf = 0x0100;         /* 320 Hz: (round(log2(320 / 10) * 32) - 0x60) * 4 */
    const uint8_t lf = 0x40;            /* 160 Hz: round(log2(160 / 10) * 32) - 0x40 */
    uint8_t ha = (uint8_t)(sw_amp(hi_amp) * 2);
    uint16_t la = (uint16_t)(sw_amp(lo_amp) / 2 + 64);
    out[0] = (uint8_t)(hf & 0xFF);
    out[1] = (uint8_t)(ha + (hf >> 8));
    out[2] = (uint8_t)(lf + (la >> 8));
    out[3] = (uint8_t)(la & 0xFF);
}

/* Report 0x01 = rumble + a sub-command, 0x10 = rumble only */
static void sw_send(lx_hidpad_t *h, const lx_pad_out_state_t *s, int sub, uint8_t arg)
{
    uint8_t r[12] = { 0 };
    r[0] = sub >= 0 ? 0x01 : 0x10;
    r[1] = h->seq++ & 0x0F;
    sw_rumble(r + 2, s->weak / 65535.0f, s->strong / 65535.0f);
    memcpy(r + 6, r + 2, 4);            /* left and right motor alike */
    size_t n = 10;
    if (sub >= 0) { r[10] = (uint8_t)sub; r[11] = arg; n = 12; }
    h->send(h->send_ctx, r, n);
}

static void switch_out(lx_hidpad_t *h, const lx_pad_out_state_t *s)
{
    if (!h->out_ready) {
        static const lx_pad_out_state_t still = { 0 };
        sw_send(h, &still, 0x48, 0x01);                      /* enable vibration */
        vTaskDelay(pdMS_TO_TICKS(30));
        sw_send(h, &still, 0x30, (uint8_t)(1u << (s->player & 3)));   /* player LED */
        vTaskDelay(pdMS_TO_TICKS(30));
        h->out_ready = true;
    }
    sw_send(h, s, -1, 0);
}

static void hidpad_out(void *ctx, const lx_pad_out_state_t *s)
{
    lx_hidpad_t *h = ctx;
    if (!h->send) return;
    if (h->kind == K_SONY) sony_out(h, s);
    else if (h->kind == K_SWITCH) switch_out(h, s);
    else if (h->kind == K_WII) lx_wii_out(&h->wii, s);
}

void lx_input_hidpad_set_sender(lx_hidpad_t *h, lx_hidpad_send_fn fn, void *ctx)
{
    if (!h) return;
    if (!fn) lx_input_pad_set_out(h->player, NULL, NULL);   /* waits for a send in progress */
    h->send = fn;
    h->send_ctx = ctx;
    if (fn && h->kind == K_WII) lx_wii_start(&h->wii, fn, ctx, h->wii.pro);   /* asks for its status: set-up */
    if (fn && (h->kind == K_SONY || h->kind == K_SWITCH || h->kind == K_WII))
        lx_input_pad_set_out(h->player, hidpad_out, h);
}

void *lx_input_hidpad_sender_ctx(lx_hidpad_t *h)
{
    return h ? h->send_ctx : NULL;
}

/* Sony full reports: gyro and accelerometer (DS4 0x11, DualSense 0x31, both 8192 per g) */
static void sony_motion(lx_hidpad_t *h, const uint8_t *p, size_t len)
{
    int at = p[0] == 0x11 ? 15 : p[0] == 0x31 ? 17 : 0;
    if (!at || len < (size_t)at + 12) return;
    papp_motion_t m = { .valid = PAPP_MOTION_ACCEL | PAPP_MOTION_GYRO, .ir_x = 0xFFFF, .ir_y = 0xFFFF,
                        .time_ms = (uint32_t)(esp_timer_get_time() / 1000) };
    for (int i = 0; i < 3; i++) {
        m.gyro[i] = (int16_t)(p[at + 2 * i] | p[at + 2 * i + 1] << 8);
        m.accel[i] = (int16_t)((int16_t)(p[at + 6 + 2 * i] | p[at + 7 + 2 * i] << 8) / 2);
    }
    lx_input_pad_motion(h->player, &m);
}

lx_hidpad_t *lx_input_hidpad_open(const char *name, uint16_t vid, uint16_t pid, const uint8_t *desc, size_t len)
{
    lx_hidpad_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->vid = vid;
    h->pid = pid;
    h->has_layout = desc && len && hid_layout_parse(desc, len, &h->layout);
    hid_state_init(&h->raw);
    if (vid == 0x054C) h->kind = K_SONY;
    else if (vid == 0x057E && pid == 0x2009) h->kind = K_SWITCH;
    else if (vid == 0x057E && (pid == 0x0306 || pid == 0x0330)) h->kind = K_WII;
    else h->kind = h->has_layout ? K_HID : K_NONE;
    h->pkind = h->kind == K_HID ? lx_pad_kind(&h->layout) : PAPP_PAD_GAMEPAD;
    if (h->kind == K_WII) {
        h->wii.pro = name && strstr(name, "-UC");           /* "Nintendo RVL-CNT-01-UC": Wii U Pro */
        h->pkind = h->wii.pro ? PAPP_PAD_GAMEPAD : PAPP_PAD_WIIMOTE;
    }
    h->player = lx_input_pad_attach_kind(name, vid, pid, h->pkind);
    if (h->player >= 0 && h->kind == K_HID) {
        int k = lx_input_pad_ext(h->player, &h->ext);
        if (k >= 0) h->pkind = k;
        h->ext_gen = lx_input_pad_ext_gen(h->player);
    }
    if (h->player < 0) {
        free(h);
        return NULL;
    }
    static const char *const kinds[] = { "HID descriptor", "Sony", "Switch Pro", "unknown format", "Wii" };
    ESP_LOGI(TAG, "player %d: %s, read as %s", h->player + 1, name, kinds[h->kind]);
    return h;
}

/* an input change someone made: a button, or a stick / trigger moved past its noise */
static bool moved(const lx_pad_state_t *a, const lx_pad_state_t *b)
{
    const int D = 3000;                     /* of +-32767: about 9 % */
    return a->b != b->b || abs(a->lx - b->lx) > D || abs(a->ly - b->ly) > D || abs(a->rx - b->rx) > D ||
           abs(a->ry - b->ry) > D || abs((int)a->lt - (int)b->lt) > 2 * D || abs((int)a->rt - (int)b->rt) > 2 * D;   /* triggers 0..65535 */
}

uint32_t lx_input_hidpad_still_ms(lx_hidpad_t *h)
{
    if (!h) return UINT32_MAX;
    return h->t_change ? (uint32_t)((esp_timer_get_time() - h->t_change) / 1000) : 0;
}

void lx_input_hidpad_report(lx_hidpad_t *h, const uint8_t *p, size_t len)
{
    if (!h || !p || !len) return;
    lx_pad_state_t s;
    bool ok = false;
    switch (h->kind) {
    case K_SONY:   ok = decode_sony(p, len, &s); break;
    case K_SWITCH: ok = decode_switch(p, len, &s); break;
    case K_HID:    ok = lx_pad_layout(&h->layout, h->pkind, &h->raw, p, len, &s); break;
    case K_WII: {
        papp_motion_t m;
        bool has_m;
        ok = lx_wii_report(&h->wii, p, len, &s, &m, &has_m);
        if (has_m) lx_input_pad_motion(h->player, &m);
        if (h->wii.kind_changed) {
            h->wii.kind_changed = false;
            lx_input_pad_set_kind(h->player, lx_wii_kind(&h->wii));
        }
        break;
    }
    default:       break;
    }
    bool by_layout = h->kind == K_HID;
    if (!ok && h->kind != K_HID && h->kind != K_WII && h->has_layout) by_layout = ok = lx_pad_layout(&h->layout, h->pkind, &h->raw, p, len, &s);
    if (ok && h->kind == K_SONY) sony_motion(h, p, len);
    if (p[0] != h->last_id) {               /* once per report kind: what the pad sends */
        h->last_id = p[0];
        ESP_LOGI(TAG, "player %d: report %02X, %u bytes%s", h->player + 1, p[0], (unsigned)len, ok ? "" : ", not read");
    }
    int64_t now = esp_timer_get_time();
    if (h->t_last) {
        uint32_t g = (uint32_t)(now - h->t_last);
        if (g > h->gap_max_us) h->gap_max_us = g;
        if (g >= LX_HIDPAD_STALL_US) {
            h->stalls++;
            if (h->t_change && h->t_last - h->t_change < LX_HIDPAD_IN_USE_US) h->stalls_use++;
        }
    }
    if (ok && (!h->have_prev || moved(&h->prev, &s))) {
        h->t_change = now;                  /* the first report counts: the pad was just switched on */
        h->prev = s;
        h->have_prev = true;
    }
    if (!h->t_window) h->t_window = now;
    h->t_last = now;
    h->n++;
    if (!ok) return;
    if (by_layout) {
        uint32_t gen = lx_input_pad_ext_gen(h->player);
        if (gen != h->ext_gen) {
            h->ext_gen = gen;
            lx_input_pad_ext(h->player, &h->ext);
        }
        lx_pad_remap(&h->ext, &h->layout, &h->raw, &s);
    }
    lx_input_pad_update(h->player, s.b, s.lx, s.ly, s.rx, s.ry, s.lt, s.rt);
    if (by_layout && h->kind == K_HID) {
        papp_joystick_t j = { 0 };
        lx_pad_joystick(&h->layout, h->pkind, &h->raw, &j);
        lx_input_pad_joystick(h->player, &j);
    }
}

void lx_input_hidpad_stats(lx_hidpad_t *h, lx_hidpad_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!h) return;
    int64_t now = esp_timer_get_time();
    out->player = h->player;
    out->reports = h->n;
    out->ms = h->t_window ? (uint32_t)((now - h->t_window) / 1000) : 0;
    out->gap_max_ms = h->gap_max_us / 1000;
    out->stalls = h->stalls;
    out->stalls_in_use = h->stalls_use;
    out->idle_ms = h->t_last ? (uint32_t)((now - h->t_last) / 1000) : 0;
    out->still_ms = lx_input_hidpad_still_ms(h);
    h->n = h->gap_max_us = h->stalls = h->stalls_use = 0;
    h->t_window = now;
}

void lx_input_hidpad_close(lx_hidpad_t *h)
{
    if (!h) return;
    lx_input_pad_detach(h->player);         /* clears the outputs first: no send after this */
    free(h);
}
