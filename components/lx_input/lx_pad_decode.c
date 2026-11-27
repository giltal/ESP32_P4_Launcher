/*
 * Gamepad report decoding shared by the USB HID source (lx_input_usb.c) and the HID pads of other
 * transports (lx_input_hidpad.c, Bluetooth).
 */
#include "lx_input_priv.h"
#include "papp_abi.h"

#include <string.h>

uint32_t lx_pad_hat(uint8_t hat)
{
    static const uint32_t map[8] = {
        PAPP_BTN_UP, PAPP_BTN_UP | PAPP_BTN_RIGHT, PAPP_BTN_RIGHT, PAPP_BTN_DOWN | PAPP_BTN_RIGHT,
        PAPP_BTN_DOWN, PAPP_BTN_DOWN | PAPP_BTN_LEFT, PAPP_BTN_LEFT, PAPP_BTN_UP | PAPP_BTN_LEFT,
    };
    return hat <= 7 ? map[hat] : 0;
}

uint32_t lx_pad_axes_dpad(uint8_t x, uint8_t y, uint8_t lo, uint8_t hi)
{
    uint32_t d = 0;
    if (y < lo) d |= PAPP_BTN_UP;
    else if (y > hi) d |= PAPP_BTN_DOWN;
    if (x < lo) d |= PAPP_BTN_LEFT;
    else if (x > hi) d |= PAPP_BTN_RIGHT;
    return d;
}

uint32_t lx_pad_ps_buttons(uint8_t hat_btn, uint8_t b2, uint8_t b3)
{
    uint32_t b = 0;
    if (hat_btn & 0x10) b |= PAPP_BTN_X;        /* Square   */
    if (hat_btn & 0x20) b |= PAPP_BTN_A;        /* Cross    */
    if (hat_btn & 0x40) b |= PAPP_BTN_B;        /* Circle   */
    if (hat_btn & 0x80) b |= PAPP_BTN_Y;        /* Triangle */
    if (b2 & 0x01) b |= PAPP_BTN_L1;
    if (b2 & 0x02) b |= PAPP_BTN_R1;
    if (b2 & 0x04) b |= PAPP_BTN_L2;
    if (b2 & 0x08) b |= PAPP_BTN_R2;
    if (b2 & 0x10) b |= PAPP_BTN_SELECT;
    if (b2 & 0x20) b |= PAPP_BTN_START;
    if (b2 & 0x40) b |= PAPP_BTN_L3;
    if (b2 & 0x80) b |= PAPP_BTN_R3;
    if (b3 & 0x01) b |= PAPP_BTN_HOME;
    if (b3 & 0x02) b |= PAPP_BTN_MISC;
    return b;
}

#define AX(v)   ((int16_t)(((int)(v) - 128) * 256))
#define TRIG(v) ((uint16_t)((v) * 257))

void lx_pad_ps4(const uint8_t *p, lx_pad_state_t *s)
{
    /* [id][LX][LY][RX][RY][hat+btn1][btn2][btn3][L2][R2] */
    s->lx = AX(p[1]); s->ly = AX(p[2]); s->rx = AX(p[3]); s->ry = AX(p[4]);
    s->b = lx_pad_hat(p[5] & 0x0F) | lx_pad_ps_buttons(p[5], p[6], p[7]);
    s->lt = TRIG(p[8]); s->rt = TRIG(p[9]);
}

void lx_pad_ps5(const uint8_t *p, lx_pad_state_t *s)
{
    /* [id][LX][LY][RX][RY][L2][R2][cnt][hat+btn1][btn2][btn3] */
    s->lx = AX(p[1]); s->ly = AX(p[2]); s->rx = AX(p[3]); s->ry = AX(p[4]);
    s->lt = TRIG(p[5]); s->rt = TRIG(p[6]);
    s->b = lx_pad_hat(p[8] & 0x0F) | lx_pad_ps_buttons(p[8], p[9], p[10]);
}

/* Default button order of generic pads; a saved mapping can change any of it */
static const uint32_t k_hid_buttons[] = {
    PAPP_BTN_X, PAPP_BTN_A, PAPP_BTN_B, PAPP_BTN_Y, PAPP_BTN_L1, PAPP_BTN_R1, PAPP_BTN_L2,
    PAPP_BTN_R2, PAPP_BTN_SELECT, PAPP_BTN_START, PAPP_BTN_L3, PAPP_BTN_R3, PAPP_BTN_HOME,
    PAPP_BTN_MISC,
};

_Static_assert((int)HID_AX_X == (int)PAPP_AXIS_X && (int)HID_AX_SLIDER == (int)PAPP_AXIS_SLIDER &&
               (int)HID_AX_THROTTLE == (int)PAPP_AXIS_THROTTLE && (int)HID_AX_SLIDER2 == (int)PAPP_AXIS_SLIDER2 &&
               (int)HID_AX_COUNT == (int)PAPP_AXIS_LT, "HID_AX_ and PAPP_AXIS_ must match");

/* Flight sticks: trigger, thumb button, then the base buttons. No HOME: a stick has no such button */
static const uint32_t k_stick_buttons[] = {
    PAPP_BTN_A, PAPP_BTN_B, PAPP_BTN_X, PAPP_BTN_Y, PAPP_BTN_L1, PAPP_BTN_R1, PAPP_BTN_L2,
    PAPP_BTN_R2, PAPP_BTN_SELECT, PAPP_BTN_START, PAPP_BTN_L3, PAPP_BTN_R3, PAPP_BTN_MISC,
};

int lx_pad_kind(const hid_layout_t *l)
{
    bool x = hid_has(l, HID_AX_X), y = hid_has(l, HID_AX_Y);
    bool throttle = hid_has(l, HID_AX_THROTTLE) || hid_has(l, HID_AX_SLIDER);
    if (hid_has(l, HID_AX_STEERING) || hid_has(l, HID_AX_ACCEL) || hid_has(l, HID_AX_BRAKE) ||
        hid_has(l, HID_AX_WHEEL))
        return PAPP_PAD_WHEEL;
    if (l->device == HID_DEV_GAMEPAD) return PAPP_PAD_GAMEPAD;
    if (x && y && (throttle || hid_has(l, HID_AX_RUDDER))) return PAPP_PAD_FLIGHTSTICK;
    if (!(x && y) && (throttle || hid_has(l, HID_AX_Z))) return PAPP_PAD_THROTTLE;
    /* a stick that twists (Rz) with neither a second stick (Z/Rx/Ry) nor triggers */
    if (x && y && hid_has(l, HID_AX_RZ) && !hid_has(l, HID_AX_Z) && !hid_has(l, HID_AX_RX) &&
        !hid_has(l, HID_AX_RY) && l->device == HID_DEV_JOYSTICK)
        return PAPP_PAD_FLIGHTSTICK;
    return PAPP_PAD_GAMEPAD;
}

/* The throttle of a stick or throttle unit: Throttle, else Slider, else Z. -1 = none */
static int throttle_axis(const hid_layout_t *l)
{
    if (hid_has(l, HID_AX_THROTTLE)) return HID_AX_THROTTLE;
    if (hid_has(l, HID_AX_SLIDER)) return HID_AX_SLIDER;
    if (hid_has(l, HID_AX_Z)) return HID_AX_Z;
    return -1;
}

/* An axis over its whole travel as a trigger, 0..65535; inverted = the low end is full */
static uint16_t as_trigger(int16_t v, bool inverted)
{
    return inverted ? (uint16_t)(32767 - v) : (uint16_t)(v + 32768);
}

static uint32_t stick_dpad(const hid_state_t *s)
{
    uint32_t b = 0;
    if (s->axis[HID_AX_Y] < -16000) b |= PAPP_BTN_UP;
    else if (s->axis[HID_AX_Y] > 16000) b |= PAPP_BTN_DOWN;
    if (s->axis[HID_AX_X] < -16000) b |= PAPP_BTN_LEFT;
    else if (s->axis[HID_AX_X] > 16000) b |= PAPP_BTN_RIGHT;
    return b;
}

void lx_pad_std(const hid_layout_t *l, int kind, const hid_state_t *s, lx_pad_state_t *out)
{
    memset(out, 0, sizeof(*out));
    bool pad_order = kind == PAPP_PAD_GAMEPAD || kind == PAPP_PAD_WHEEL;
    const uint32_t *order = pad_order ? k_hid_buttons : k_stick_buttons;
    size_t norder = pad_order ? sizeof(k_hid_buttons) / sizeof(k_hid_buttons[0])
                              : sizeof(k_stick_buttons) / sizeof(k_stick_buttons[0]);
    uint32_t b = s->hat[0] >= 0 ? lx_pad_hat((uint8_t)s->hat[0]) : 0;
    for (size_t i = 0; i < norder; i++)
        if (s->buttons & (1u << i)) b |= order[i];

    switch (kind) {
    case PAPP_PAD_FLIGHTSTICK:
    case PAPP_PAD_THROTTLE: {
        int t = throttle_axis(l);
        if (kind == PAPP_PAD_FLIGHTSTICK) {
            out->lx = s->axis[HID_AX_X];
            out->ly = s->axis[HID_AX_Y];
        } else {                            /* a throttle's mini stick, when it has one */
            out->lx = s->axis[HID_AX_RX];
            out->ly = s->axis[HID_AX_RY];
        }
        out->rx = hid_has(l, HID_AX_RZ) ? s->axis[HID_AX_RZ] : s->axis[HID_AX_RUDDER];
        if (t >= 0) {                       /* pushed forward = low values = up = full */
            out->ry = s->axis[t];
            out->rt = as_trigger(s->axis[t], true);
        }
        if (!l->hats && kind == PAPP_PAD_FLIGHTSTICK) b |= stick_dpad(s);
        break;
    }
    case PAPP_PAD_WHEEL: {
        int st = hid_has(l, HID_AX_STEERING) ? HID_AX_STEERING : hid_has(l, HID_AX_WHEEL) ? HID_AX_WHEEL : HID_AX_X;
        out->lx = s->axis[st];
        if (st != HID_AX_X) out->ly = s->axis[HID_AX_Y];
        /* pedals: Accelerator / Brake, else Rz (gas) and Y or Z (brake) as older wheels have them */
        if (hid_has(l, HID_AX_ACCEL)) out->rt = as_trigger(s->axis[HID_AX_ACCEL], false);
        else if (hid_has(l, HID_AX_RZ)) out->rt = as_trigger(s->axis[HID_AX_RZ], true);
        if (hid_has(l, HID_AX_BRAKE)) out->lt = as_trigger(s->axis[HID_AX_BRAKE], false);
        else if (st == HID_AX_X && hid_has(l, HID_AX_Y) && hid_has(l, HID_AX_RZ))
            out->lt = as_trigger(s->axis[HID_AX_Y], true);
        else if (hid_has(l, HID_AX_Z)) out->lt = as_trigger(s->axis[HID_AX_Z], true);
        break;
    }
    default: {
        if (!l->hats) b |= stick_dpad(s);
        /* Right stick is Z/Rz when present, then Rx/Ry are the triggers */
        bool zrz = hid_has(l, HID_AX_Z) && hid_has(l, HID_AX_RZ);
        bool trig = zrz && hid_has(l, HID_AX_RX) && hid_has(l, HID_AX_RY);
        out->lx = s->axis[HID_AX_X];
        out->ly = s->axis[HID_AX_Y];
        out->rx = zrz ? s->axis[HID_AX_Z] : s->axis[HID_AX_RX];
        out->ry = zrz ? s->axis[HID_AX_RZ] : s->axis[HID_AX_RY];
        out->lt = trig ? (uint16_t)(s->axis[HID_AX_RX] + 32768) : 0;
        out->rt = trig ? (uint16_t)(s->axis[HID_AX_RY] + 32768) : 0;
        break;
    }
    }
    out->b = b;
}

/* gamepads.txt axis lines: an axis of the device to a stick or trigger of the standard view */
void lx_pad_remap(const lx_padmap_ext_t *e, const hid_layout_t *l, const hid_state_t *s, lx_pad_state_t *out)
{
    if (!e) return;
    for (int i = 0; i < e->naxis; i++) {
        int role = e->axis[i].role, to = e->axis[i].to;
        if (role >= HID_AX_COUNT || !hid_has(l, role) || to > LX_PADAX_RT) continue;
        int16_t v = s->axis[role];
        if (e->axis[i].inv) v = v == -32768 ? 32767 : (int16_t)-v;
        switch (to) {
        case LX_PADAX_LX: out->lx = v; break;
        case LX_PADAX_LY: out->ly = v; break;
        case LX_PADAX_RX: out->rx = v; break;
        case LX_PADAX_RY: out->ry = v; break;
        case LX_PADAX_LT: out->lt = as_trigger(v, false); break;
        case LX_PADAX_RT: out->rt = as_trigger(v, false); break;
        }
    }
}

bool lx_pad_layout(const hid_layout_t *l, int kind, hid_state_t *raw, const uint8_t *p, size_t len,
                   lx_pad_state_t *out)
{
    if (!hid_layout_decode(l, p, len, raw)) return false;
    lx_pad_std(l, kind, raw, out);
    return true;
}

void lx_pad_joystick(const hid_layout_t *l, int kind, const hid_state_t *s, papp_joystick_t *j)
{
    j->kind = (uint8_t)kind;
    j->naxes = 0;
    for (int a = 0; a < HID_AX_COUNT && j->naxes < PAPP_JOY_AXES; a++) {
        if (!hid_has(l, a)) continue;
        j->role[j->naxes] = (uint8_t)a;
        j->axis[j->naxes++] = s->axis[a];
    }
    j->nhats = l->hats;
    for (int h = 0; h < PAPP_JOY_HATS; h++) j->hat[h] = h < l->hats ? s->hat[h] : -1;
    j->nbuttons = l->buttons;
    j->buttons = s->buttons;
}

void lx_pad_joystick_std(const lx_pad_state_t *s, int kind, papp_joystick_t *j)
{
    static const uint8_t roles[] = { PAPP_AXIS_X, PAPP_AXIS_Y, PAPP_AXIS_RX, PAPP_AXIS_RY, PAPP_AXIS_LT, PAPP_AXIS_RT };
    int16_t v[] = { s->lx, s->ly, s->rx, s->ry, (int16_t)(s->lt - 32768), (int16_t)(s->rt - 32768) };
    j->kind = (uint8_t)kind;
    j->naxes = 6;
    for (int i = 0; i < 6; i++) { j->role[i] = roles[i]; j->axis[i] = v[i]; }
    j->nhats = 0;
    j->hat[0] = j->hat[1] = -1;
    j->nbuttons = 20;
    j->buttons = s->b;
}

uint8_t lx_pad_ps5_leds(int player)
{
    static const uint8_t k[4] = { 0x04, 0x0A, 0x15, 0x1B };     /* the 5 LEDs under the touchpad */
    return k[player & 3];
}
