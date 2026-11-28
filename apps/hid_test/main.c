/*
 * hid_test — lx_hid_parse against typical gamepad, flight stick, throttle and wheel descriptors, and
 * lx_pad_decode's device kinds and standard-pad views. Exit code = failures.
 * Also the Wii Remote set-up and reports (lx_pad_wii.c) against a scripted remote.
 *   python papp-sdk/tools/papp_build.py --name hid_test --libc --inc components/lx_input components/lx_input/include apps/hid_test/shim \
 *       --src apps/hid_test/main.c components/lx_input/lx_hid_parse.c components/lx_input/lx_pad_decode.c \
 *       components/lx_input/lx_pad_wii.c
 */
#include <string.h>
#include "papp/papp.h"
#include "lx_hid_parse.h"
#include "lx_input_priv.h"

PAPP_MANIFEST(.id = "test.hid", .name = "HID test", .version = PAPP_APP_VERSION, .main_stack = 16384,
              .flags = PAPP_MF_HIDDEN);

static int s_fail;
#define CHECK(c) do { if (!(c)) { papp_log("FAIL line %d: %s\n", __LINE__, #c); s_fail++; } } while (0)

/* DragonRise 0079:0006 style: no report id, 5 axes, hat, 12 buttons, vendor bits */
static const uint8_t k_generic[] = {
    0x05,0x01, 0x09,0x04, 0xA1,0x01, 0xA1,0x02,
    0x75,0x08, 0x95,0x05, 0x15,0x00, 0x26,0xFF,0x00, 0x35,0x00, 0x46,0xFF,0x00,
    0x09,0x30, 0x09,0x31, 0x09,0x32, 0x09,0x32, 0x09,0x35, 0x81,0x02,
    0x75,0x04, 0x95,0x01, 0x25,0x07, 0x46,0x3B,0x01, 0x65,0x14, 0x09,0x39, 0x81,0x42,
    0x65,0x00, 0x75,0x01, 0x95,0x0C, 0x25,0x01, 0x45,0x01, 0x05,0x09, 0x19,0x01, 0x29,0x0C, 0x81,0x02,
    0x06,0x00,0xFF, 0x75,0x01, 0x95,0x08, 0x25,0x01, 0x45,0x01, 0x09,0x01, 0x81,0x02,
    0xC0, 0xC0,
};

/* Report id 1, hat first, buttons, 16-bit signed axes; a vendor output report */
static const uint8_t k_signed[] = {
    0x05,0x01, 0x09,0x05, 0xA1,0x01, 0x85,0x01,
    0x05,0x01, 0x09,0x39, 0x15,0x01, 0x25,0x08, 0x75,0x04, 0x95,0x01, 0x81,0x42,
    0x75,0x04, 0x95,0x01, 0x81,0x03,
    0x05,0x09, 0x19,0x01, 0x29,0x0A, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x0A, 0x81,0x02,
    0x75,0x06, 0x95,0x01, 0x81,0x03,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x09,0x33, 0x09,0x34, 0x16,0x00,0x80, 0x26,0xFF,0x7F,
    0x75,0x10, 0x95,0x04, 0x81,0x02,
    0x85,0x02, 0x06,0x00,0xFF, 0x09,0x01, 0x75,0x08, 0x95,0x10, 0x91,0x02,
    0xC0,
};

/* A mouse, which must be refused */
static const uint8_t k_mouse[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x09,0x01, 0xA1,0x00, 0x05,0x09, 0x19,0x01, 0x29,0x03,
    0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x03, 0x81,0x02, 0x75,0x05, 0x95,0x01, 0x81,0x03,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x02, 0x81,0x06,
    0xC0, 0xC0,
};

/* A pad in report id 3, after a keyboard collection in report id 1 */
static const uint8_t k_multi[] = {
    0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x85,0x01, 0x05,0x07, 0x19,0xE0, 0x29,0xE7, 0x15,0x00,
    0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02, 0xC0,
    0x05,0x01, 0x09,0x05, 0xA1,0x01, 0x85,0x03,
    0x09,0x30, 0x09,0x31, 0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x02, 0x81,0x02,
    0x05,0x09, 0x19,0x01, 0x29,0x08, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
    0xC0,
};

/* Flight stick in the style of the common 4-axis sticks: X/Y 10 bit, hat, twist (Rz), 12 buttons,
 * throttle on a Slider. No report id */
static const uint8_t k_stick[] = {
    0x05,0x01, 0x09,0x04, 0xA1,0x01, 0xA1,0x02,
    0x75,0x0A, 0x95,0x02, 0x15,0x00, 0x26,0xFF,0x03, 0x09,0x30, 0x09,0x31, 0x81,0x02,
    0x75,0x04, 0x95,0x01, 0x25,0x07, 0x46,0x3B,0x01, 0x65,0x14, 0x09,0x39, 0x81,0x42,
    0x65,0x00, 0x75,0x08, 0x95,0x01, 0x26,0xFF,0x00, 0x09,0x35, 0x81,0x02,
    0x05,0x09, 0x15,0x00, 0x25,0x01, 0x19,0x01, 0x29,0x0C, 0x75,0x01, 0x95,0x0C, 0x81,0x02,
    0x75,0x04, 0x95,0x01, 0x81,0x01,
    0x05,0x01, 0x09,0x36, 0x75,0x08, 0x95,0x01, 0x26,0xFF,0x00, 0x81,0x02,
    0xC0, 0xC0,
};

/* 3-axis stick, Logitech Attack 3 style: X, Y, and the throttle as Z, 8 bit; 11 buttons, no hat */
static const uint8_t k_stick3[] = {
    0x05,0x01, 0x09,0x04, 0xA1,0x01, 0xA1,0x02,
    0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x03, 0x09,0x30, 0x09,0x31, 0x09,0x32, 0x81,0x02,
    0x05,0x09, 0x15,0x00, 0x25,0x01, 0x19,0x01, 0x29,0x0B, 0x75,0x01, 0x95,0x0B, 0x81,0x02,
    0x75,0x05, 0x95,0x01, 0x81,0x01,
    0xC0, 0xC0,
};

/* Throttle unit: Simulation-page throttle and rudder plus 2 hats in report 1, 20 buttons in report 2 */
static const uint8_t k_throttle[] = {
    0x05,0x01, 0x09,0x04, 0xA1,0x01,
    0x85,0x01,
    0x05,0x02, 0x09,0xBB, 0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x01, 0x81,0x02,
    0x09,0xBA, 0x81,0x02,
    0x05,0x01, 0x09,0x39, 0x09,0x39, 0x15,0x00, 0x25,0x07, 0x75,0x04, 0x95,0x02, 0x81,0x42,
    0x85,0x02,
    0x05,0x09, 0x19,0x01, 0x29,0x14, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x14, 0x81,0x02,
    0x75,0x04, 0x95,0x01, 0x81,0x01,
    0xC0,
};

/* Wheel: signed 16-bit steering, accelerator and brake pedals (Simulation page), 14 buttons */
static const uint8_t k_wheel[] = {
    0x05,0x01, 0x09,0x04, 0xA1,0x01,
    0x05,0x02, 0x09,0xC8, 0x16,0x00,0x80, 0x26,0xFF,0x7F, 0x75,0x10, 0x95,0x01, 0x81,0x02,
    0x09,0xC4, 0x09,0xC5, 0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x02, 0x81,0x02,
    0x05,0x09, 0x19,0x01, 0x29,0x0E, 0x25,0x01, 0x75,0x01, 0x95,0x0E, 0x81,0x02,
    0x75,0x02, 0x95,0x01, 0x81,0x01,
    0xC0,
};

/* Multi-axis controller (0x08) whose X/Y come as extended usages while the page is Button */
static const uint8_t k_multiaxis[] = {
    0x05,0x01, 0x09,0x08, 0xA1,0x01,
    0x05,0x09, 0x0B,0x30,0x00,0x01,0x00, 0x0B,0x31,0x00,0x01,0x00, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x02, 0x81,0x02,
    0x19,0x01, 0x29,0x02, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x02, 0x81,0x02,
    0x75,0x06, 0x95,0x01, 0x81,0x01,
    0xC0,
};

/* What lx_pad_wii.c sent to the "remote" */
static uint8_t s_sent[12][24];
static int s_nsent;

static void fake_send(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (s_nsent < 12) memcpy(s_sent[s_nsent++], d, n < 24 ? n : 24);
}

static void wii_tests(void)
{
    lx_wii_t w;
    lx_pad_state_t ws;
    papp_motion_t m;
    bool hm;

    /* A remote with a Nunchuk: status -> 2 register writes -> id read -> data mode 0x35 */
    s_nsent = 0;
    lx_wii_start(&w, fake_send, NULL, false);
    CHECK(s_nsent == 1 && s_sent[0][0] == 0x15);
    const uint8_t st[] = { 0x20, 0x00, 0x00, 0x02, 0x00, 0x00, 0xC8 };
    lx_wii_report(&w, st, sizeof(st), &ws, &m, &hm);
    CHECK(s_nsent == 2 && s_sent[1][0] == 0x16 && s_sent[1][2] == 0xA4 && s_sent[1][4] == 0xF0 && s_sent[1][6] == 0x55);
    const uint8_t ack[] = { 0x22, 0x00, 0x00, 0x16, 0x00 };
    lx_wii_report(&w, ack, sizeof(ack), &ws, &m, &hm);
    CHECK(s_nsent == 3 && s_sent[2][0] == 0x16 && s_sent[2][4] == 0xFB && s_sent[2][6] == 0x00);
    lx_wii_report(&w, ack, sizeof(ack), &ws, &m, &hm);
    CHECK(s_nsent == 4 && s_sent[3][0] == 0x17 && s_sent[3][4] == 0xFA && s_sent[3][6] == 6);
    uint8_t rd[22] = { 0x21, 0x00, 0x00, 0x50, 0x00, 0xFA, 0x00, 0x00, 0xA4, 0x20, 0x00, 0x00 };
    lx_wii_report(&w, rd, sizeof(rd), &ws, &m, &hm);
    CHECK(w.kind_changed && lx_wii_kind(&w) == PAPP_PAD_WIIMOTE_NUNCHUK);
    CHECK(s_nsent == 5 && s_sent[4][0] == 0x12 && s_sent[4][2] == 0x35);
    /* A pressed; accelerometer z 0x99; Nunchuk stick full right, Z pressed */
    const uint8_t d[22] = { 0x35, 0x00, 0x08, 0x80, 0x80, 0x99, 228, 128, 0x80, 0x80, 0x80, 0xFE };
    CHECK(lx_wii_report(&w, d, sizeof(d), &ws, &m, &hm));
    CHECK(ws.b == (PAPP_BTN_A | PAPP_BTN_L2) && ws.lx == 32767 && ws.ly == 0 && ws.lt == 65535);
    CHECK(hm && m.accel[0] == 0 && m.accel[2] == (0x99 * 4 - 512) * 41);
    /* rumble on: the LED report carries it, and so does every report after it */
    lx_pad_out_state_t o = { .strong = 40000, .player = 1 };
    lx_wii_out(&w, &o);
    CHECK(s_nsent == 6 && s_sent[5][0] == 0x11 && s_sent[5][1] == (0x20 | 1));

    /* A remote alone, held sideways */
    s_nsent = 0;
    lx_wii_start(&w, fake_send, NULL, false);
    const uint8_t st0[] = { 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64 };
    lx_wii_report(&w, st0, sizeof(st0), &ws, &m, &hm);
    CHECK(s_nsent == 2 && s_sent[1][0] == 0x12 && s_sent[1][2] == 0x31 && lx_wii_kind(&w) == PAPP_PAD_WIIMOTE);
    const uint8_t d0[] = { 0x31, 0x08, 0x01, 0x80, 0x80, 0x80 };   /* d-pad up, 2 */
    CHECK(lx_wii_report(&w, d0, sizeof(d0), &ws, &m, &hm));
    CHECK(ws.b == (PAPP_BTN_LEFT | PAPP_BTN_A));

    /* Classic Controller: B (south) and ZR pressed (active low), left stick centred */
    s_nsent = 0;
    lx_wii_start(&w, fake_send, NULL, false);
    lx_wii_report(&w, st, sizeof(st), &ws, &m, &hm);
    lx_wii_report(&w, ack, sizeof(ack), &ws, &m, &hm);
    lx_wii_report(&w, ack, sizeof(ack), &ws, &m, &hm);
    uint8_t rc[22] = { 0x21, 0x00, 0x00, 0x50, 0x00, 0xFA, 0x00, 0x00, 0xA4, 0x20, 0x01, 0x01 };
    lx_wii_report(&w, rc, sizeof(rc), &ws, &m, &hm);
    CHECK(lx_wii_kind(&w) == PAPP_PAD_GAMEPAD);
    const uint8_t dc[22] = { 0x35, 0x00, 0x00, 0x80, 0x80, 0x80, 0x20, 0x20, 0x10, 0x00, 0xFF, (uint8_t)~(0x40 | 0x04) };
    CHECK(lx_wii_report(&w, dc, sizeof(dc), &ws, &m, &hm));
    CHECK(ws.b == (PAPP_BTN_A | PAPP_BTN_R2) && ws.lx == 0 && ws.ly == 0);
}

int main(int argc, char **argv)
{
    hid_layout_t l;
    hid_state_t s;
    lx_pad_state_t st;

    CHECK(hid_layout_parse(k_generic, sizeof(k_generic), &l));
    CHECK(l.report_id == 0 && l.buttons == 12 && l.hats == 1 && l.hat[0].size == 4);
    CHECK(l.axis[HID_AX_X].bit == 0 && l.axis[HID_AX_Y].bit == 8 && l.axis[HID_AX_RZ].bit == 32);
    CHECK(l.hat[0].bit == 40 && l.button[0].bit == 44 && l.button[11].bit == 55);
    CHECK(l.device == HID_DEV_JOYSTICK && lx_pad_kind(&l) == PAPP_PAD_GAMEPAD);   /* a pad that says joystick */
    /* X=0 Y=255; byte 5: hat 2, buttons 1 and 3 in the high nibble; button 9 at bit 52 */
    const uint8_t r1[] = { 0x00, 0xFF, 0x80, 0x80, 0x80, 0x52, 0x10, 0x00, 0x00 };
    hid_state_init(&s);
    CHECK(hid_layout_decode(&l, r1, sizeof(r1), &s));
    CHECK(s.axis[HID_AX_X] == -32768 && s.axis[HID_AX_Y] == 32767);
    CHECK(s.hat[0] == 2 && s.hat[1] == -1);
    CHECK(s.buttons == (0x005 | 0x100));
    lx_pad_std(&l, PAPP_PAD_GAMEPAD, &s, &st);
    CHECK(st.b == (PAPP_BTN_X | PAPP_BTN_B | PAPP_BTN_SELECT | PAPP_BTN_RIGHT) && st.lx == -32768);

    CHECK(hid_layout_parse(k_signed, sizeof(k_signed), &l));
    CHECK(l.report_id == 1 && l.nids == 1 && l.buttons == 10 && l.hat[0].min == 1 && l.hat[0].max == 8);
    CHECK(l.axis[HID_AX_RX].bit == 56 && l.axis[HID_AX_RY].bit == 72 && l.axis[HID_AX_RX].sign);
    CHECK(l.device == HID_DEV_GAMEPAD && lx_pad_kind(&l) == PAPP_PAD_GAMEPAD);
    /* hat value 3 (min 1) = right; buttons 1, 3, 10; axes -32768, 32767, 0, 0x1234 */
    const uint8_t r2[] = { 1, 0x03, 0x05, 0x02, 0x00,0x80, 0xFF,0x7F, 0x00,0x00, 0x34,0x12 };
    hid_state_init(&s);
    CHECK(hid_layout_decode(&l, r2, sizeof(r2), &s));
    CHECK(s.hat[0] == 2 && s.buttons == (0x05 | 0x200));
    CHECK(s.axis[HID_AX_X] == -32768 && s.axis[HID_AX_Y] == 32767 && s.axis[HID_AX_RX] == 0);
    CHECK(s.axis[HID_AX_RY] == 0x1234);
    const uint8_t r3[] = { 2, 0, 0, 0 };
    CHECK(!hid_layout_decode(&l, r3, sizeof(r3), &s));
    CHECK(s.buttons == (0x05 | 0x200));     /* a report that isn't ours changes nothing */

    CHECK(!hid_layout_parse(k_mouse, sizeof(k_mouse), &l));

    CHECK(hid_layout_parse(k_multi, sizeof(k_multi), &l));
    CHECK(l.report_id == 3 && l.buttons == 8 && l.axis[HID_AX_X].bit == 0 && l.button[0].bit == 16);
    CHECK(!hid_layout_parse(k_multi, 10, &l));   /* truncated */

    /* Flight stick */
    CHECK(hid_layout_parse(k_stick, sizeof(k_stick), &l));
    CHECK(l.buttons == 12 && l.hats == 1 && hid_has(&l, HID_AX_RZ) && hid_has(&l, HID_AX_SLIDER));
    CHECK(l.axis[HID_AX_SLIDER].bit == 48 && l.button[11].bit == 43 && l.axis[HID_AX_X].max == 1023);
    CHECK(lx_pad_kind(&l) == PAPP_PAD_FLIGHTSTICK);
    /* X full right, Y full forward, hat up, twist 128, buttons 1 and 12, throttle full forward (0) */
    const uint8_t r4[] = { 0xFF, 0x03, 0x00, 0x80, 0x01, 0x08, 0x00 };
    hid_state_init(&s);
    CHECK(hid_layout_decode(&l, r4, sizeof(r4), &s));
    CHECK(s.axis[HID_AX_X] == 32767 && s.axis[HID_AX_Y] == -32768 && s.axis[HID_AX_RZ] == 128);
    CHECK(s.hat[0] == 0 && s.buttons == 0x801 && s.axis[HID_AX_SLIDER] == -32768);
    lx_pad_std(&l, PAPP_PAD_FLIGHTSTICK, &s, &st);
    CHECK(st.b == (PAPP_BTN_A | PAPP_BTN_R3 | PAPP_BTN_UP));
    CHECK(st.lx == 32767 && st.ly == -32768 && st.rx == 128 && st.ry == -32768 && st.rt == 65535);
    lx_padmap_ext_t e = { .kind = -1, .naxis = 1 };   /* gamepads.txt "SLIDER=-LT" */
    e.axis[0].role = HID_AX_SLIDER; e.axis[0].to = LX_PADAX_LT; e.axis[0].inv = true;
    lx_pad_remap(&e, &l, &s, &st);
    CHECK(st.lt == 65535 && st.rt == 65535);
    papp_joystick_t j = { 0 };
    lx_pad_joystick(&l, PAPP_PAD_FLIGHTSTICK, &s, &j);
    CHECK(j.naxes == 4 && j.role[0] == PAPP_AXIS_X && j.role[2] == PAPP_AXIS_RZ && j.role[3] == PAPP_AXIS_SLIDER);
    CHECK(j.axis[3] == -32768 && j.nbuttons == 12 && j.buttons == 0x801 && j.nhats == 1 && j.hat[0] == 0);

    /* 3-axis stick: Z is the throttle */
    CHECK(hid_layout_parse(k_stick3, sizeof(k_stick3), &l));
    CHECK(l.device == HID_DEV_JOYSTICK && l.buttons == 11 && hid_has(&l, HID_AX_Z) && !hid_has(&l, HID_AX_RZ));
    CHECK(lx_pad_kind(&l) == PAPP_PAD_FLIGHTSTICK);

    /* Throttle over two reports: each one updates only its own controls */
    CHECK(hid_layout_parse(k_throttle, sizeof(k_throttle), &l));
    CHECK(l.nids == 2 && l.ids[0] == 1 && l.ids[1] == 2 && l.buttons == 20 && l.hats == 2);
    CHECK(l.axis[HID_AX_THROTTLE].id == 1 && l.axis[HID_AX_RUDDER].bit == 8 && l.button[19].id == 2);
    CHECK(lx_pad_kind(&l) == PAPP_PAD_THROTTLE);
    const uint8_t r5[] = { 1, 0xFF, 0x00, 0x62 }, r6[] = { 2, 0x01, 0x00, 0x08 };
    hid_state_init(&s);
    CHECK(hid_layout_decode(&l, r5, sizeof(r5), &s));
    CHECK(s.axis[HID_AX_THROTTLE] == 32767 && s.axis[HID_AX_RUDDER] == -32768 && s.hat[0] == 2 && s.hat[1] == 6);
    CHECK(hid_layout_decode(&l, r6, sizeof(r6), &s));
    CHECK(s.buttons == (1u | 1u << 19) && s.axis[HID_AX_THROTTLE] == 32767 && s.hat[1] == 6);
    lx_pad_std(&l, PAPP_PAD_THROTTLE, &s, &st);
    CHECK(st.ry == 32767 && st.rt == 0 && st.rx == -32768);

    /* Wheel and pedals */
    CHECK(hid_layout_parse(k_wheel, sizeof(k_wheel), &l));
    CHECK(hid_has(&l, HID_AX_STEERING) && hid_has(&l, HID_AX_ACCEL) && hid_has(&l, HID_AX_BRAKE) && l.buttons == 14);
    CHECK(l.axis[HID_AX_STEERING].sign && l.axis[HID_AX_BRAKE].bit == 24);
    CHECK(lx_pad_kind(&l) == PAPP_PAD_WHEEL);
    const uint8_t r7[] = { 0x00, 0xC0, 0xFF, 0x00, 0x02, 0x00 };   /* steering -16384, gas full, brake off, button 2 */
    hid_state_init(&s);
    CHECK(hid_layout_decode(&l, r7, sizeof(r7), &s));
    lx_pad_std(&l, PAPP_PAD_WHEEL, &s, &st);
    CHECK(st.lx == -16384 && st.rt == 65535 && st.lt == 0 && st.b == PAPP_BTN_A);

    /* Multi-axis controller, extended usages */
    CHECK(hid_layout_parse(k_multiaxis, sizeof(k_multiaxis), &l));
    CHECK(l.device == HID_DEV_MULTIAXIS && l.axis[HID_AX_X].bit == 0 && l.axis[HID_AX_Y].bit == 8);
    CHECK(l.buttons == 2 && l.button[1].bit == 17 && l.axis[HID_AX_X].sign);

    wii_tests();

    papp_log("hid_test: %d failure(s)\n", s_fail);
    return s_fail;
}
