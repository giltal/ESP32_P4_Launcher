/*
 * Controller test — what a pad, flight stick, throttle or wheel reports: every axis by its role, the hats,
 * all its buttons by number (INPT v4 joystick), the standard-pad view apps see (papp_gamepad_t), motion
 * sensors, and rumble buttons. One tab per player. Touch the tabs; HOME (or Select+Start held) exits,
 * so every button of the device can be tried. Draws with edu_ui (apps/education/common).
 *   python apps/controller_test/build.py
 */
#include <stdio.h>
#include <string.h>
#include "papp/papp.h"
#include "edu_ui.h"

PAPP_MANIFEST(.id = "tool.controller_test", .name = "Controller test", .version = PAPP_APP_VERSION,
              .main_stack = 16384);

enum { ID_WEAK = 1, ID_STRONG, ID_BOTH, ID_STOP };

static const char *const k_role[PAPP_AXIS_COUNT] = {
    "X", "Y", "Z", "Rx", "Ry", "Rz", "Slider", "Dial", "Wheel", "Throttle", "Rudder", "Steering",
    "Accel", "Brake", "Slider 2", "LT", "RT",
};
static const char *const k_kind[] = {
    "Gamepad", "Flight stick", "Throttle", "Wheel", "Wii Remote", "Wii Remote + Nunchuk", "Arcade stick", "Controller",
};
static const char *const k_btn[20] = {
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "Select", "Start", "L3", "R3", "Home", "Misc",
    "", "", "Up", "Down", "Left", "Right",
};

static int s_tab;
static bool s_quit;
static char s_note[64];

static void act(int id, int x)
{
    (void)x;
    if (id >= UI_ID_TAB && id < UI_ID_TAB + PAPP_MAX_PLAYERS) s_tab = id - UI_ID_TAB;
    else if (id == UI_ID_CLOSE) s_quit = true;
    else if (id >= ID_WEAK && id <= ID_STOP) {
        static const uint16_t strong[] = { 0, 50000, 50000, 0 }, weak[] = { 50000, 0, 50000, 0 };
        bool ok = papp_input_rumble(s_tab, strong[id - 1], weak[id - 1], id == ID_STOP ? 0 : 600);
        snprintf(s_note, sizeof(s_note), "%s", ok ? "" : "This controller has no motors the launcher can drive");
    }
}

/* -32768..32767 as a centred bar */
static void axis_bar(int x, int y, int w, const char *label, int16_t v)
{
    char t[16];
    ui_text_l(ui_small, x, y + 11, label, C_DIM);
    int bx = x + 92, bw = w - 92 - 70;
    ui_rrect(bx, y + 4, bw, 14, 7, C_RAISED);
    int c = bx + bw / 2, p = c + (int)((int32_t)v * (bw / 2) / 32768);
    if (p >= c) ui_fill(c, y + 6, p - c + 1, 10, C_BLUE);
    else ui_fill(p, y + 6, c - p, 10, C_BLUE);
    ui_fill(c, y + 2, 1, 18, C_FAINT);
    snprintf(t, sizeof(t), "%d", v);
    ui_text_r(ui_small, x + w, y + 11, t, C_TEXT);
}

/* 0..7 clockwise from up, -1 centred */
static void hat(int cx, int cy, int r, int dir, const char *label)
{
    ui_disc(cx, cy, r, C_RAISED, 255);
    for (int d = 0; d < 8; d++) {
        static const int8_t dx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 }, dy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
        float k = d & 1 ? 0.70f : 1.0f;
        ui_disc(cx + dx[d] * k * (r - 12), cy + dy[d] * k * (r - 12), 7, d == dir ? C_GREEN : C_FAINT, 255);
    }
    ui_disc(cx, cy, 7, dir < 0 ? C_GREEN : C_FAINT, 255);
    ui_text_c(ui_small, cx, cy + r + 14, label, C_DIM);
}

static void stick(int cx, int cy, int r, int16_t x, int16_t y, const char *label)
{
    ui_disc(cx, cy, r, C_RAISED, 255);
    ui_line(cx - r, cy, cx + r, cy, 1, C_FAINT);
    ui_line(cx, cy - r, cx, cy + r, 1, C_FAINT);
    ui_disc(cx + x * (float)(r - 6) / 32768, cy + y * (float)(r - 6) / 32768, 6, C_GREEN, 255);
    ui_text_c(ui_small, cx, cy + r + 14, label, C_DIM);
}

static void trigger(int x, int y, int h, uint16_t v, const char *label)
{
    ui_rrect(x, y, 16, h, 6, C_RAISED);
    int f = (int)((uint32_t)v * (h - 4) / 65535);
    if (f) ui_fill(x + 2, y + h - 2 - f, 12, f, C_ORANGE);
    ui_text_c(ui_small, x + 8, y + h + 14, label, C_DIM);
}

static void draw_player(int pl)
{
    papp_joystick_t j;
    papp_gamepad_t g;
    if (!papp_input_joystick(pl, &j)) {
        ui_text_c(ui_med, UI_W / 2, 250, "No controller for this player", C_DIM);
        ui_text_c(ui_small, UI_W / 2, 296, "Plug one in or connect one over Bluetooth (Settings)", C_FAINT);
        return;
    }
    papp_input_gamepad(pl, &g);

    /* Left: every axis by role */
    ui_rrect(12, 112, 372, 264, 12, C_PANEL);
    ui_text_l(ui_small, 26, 128, "Axes", C_TEXT);
    for (int i = 0; i < j.naxes && i < PAPP_JOY_AXES; i++)
        axis_bar(26, 146 + i * 28, 344, j.role[i] < PAPP_AXIS_COUNT ? k_role[j.role[i]] : "?", j.axis[i]);
    if (!j.naxes) ui_text_l(ui_small, 26, 160, "none", C_FAINT);

    /* Right: buttons by number, hats */
    ui_rrect(396, 112, 392, 264, 12, C_PANEL);
    char t[48];
    if (j.flags & PAPP_JOY_STD_BUTTONS) {           /* a pad read by model: its buttons by name */
        ui_text_l(ui_small, 410, 128, "Buttons", C_TEXT);
        int k = 0;
        for (int b = 0; b < 20; b++) {
            if (!k_btn[b][0]) continue;
            int bx = 410 + (k % 5) * 74, by = 150 + (k / 5) * 34;
            bool on = j.buttons >> b & 1;
            ui_rrect(bx, by, 68, 28, 14, on ? C_GREEN : C_RAISED);
            ui_text_c(ui_small, bx + 34, by + 14, k_btn[b], on ? C_BG : C_DIM);
            k++;
        }
    } else {
        snprintf(t, sizeof(t), "Buttons (%d)", j.nbuttons);
        ui_text_l(ui_small, 410, 128, t, C_TEXT);
        int nb = j.nbuttons > 32 ? 32 : j.nbuttons;
        for (int b = 0; b < nb; b++) {
            int bx = 426 + (b % 8) * 30, by = 162 + (b / 8) * 34;
            bool on = j.buttons >> b & 1;
            ui_disc(bx, by, 13, on ? C_GREEN : C_RAISED, 255);
            snprintf(t, sizeof(t), "%d", b + 1);
            ui_text_c(ui_small, bx, by, t, on ? C_BG : C_DIM);
        }
    }
    for (int h = 0; h < j.nhats && h < PAPP_JOY_HATS; h++)
        hat(h ? 600 : 720, 170, 34, j.hat[h], h ? "Hat 2" : "Hat 1");
    if (!j.nhats && !(j.flags & PAPP_JOY_STD_BUTTONS)) ui_text_c(ui_small, 720, 180, "no hat", C_FAINT);

    /* Bottom: the standard pad apps see */
    ui_rrect(12, 384, 776, 88, 12, C_PANEL);
    stick(60, 420, 26, g.lx, g.ly, "L");
    stick(130, 420, 26, g.rx, g.ry, "R");
    trigger(176, 392, 44, g.lt, "LT");
    trigger(206, 392, 44, g.rt, "RT");
    char names[160] = "";
    size_t n = 0;
    for (int b = 0; b < 20; b++)
        if (g.buttons >> b & 1 && k_btn[b][0]) n += snprintf(names + n, sizeof(names) - n, "%s%s", n ? " " : "", k_btn[b]);
    ui_text_l(ui_small, 244, 400, "Standard pad:", C_DIM);
    ui_text_l(ui_small, 244, 426, n ? names : "-", C_TEXT);

    papp_motion_t m;
    if (papp_input_motion(pl, &m)) {
        snprintf(t, sizeof(t), "Accel %+.2f %+.2f %+.2f g", m.accel[0] / 4096.0f, m.accel[1] / 4096.0f,
                 m.accel[2] / 4096.0f);
        ui_text_l(ui_small, 244, 452, t, C_IN);
    }
    if (j.flags & PAPP_JOY_RUMBLE) {
        ui_text_l(ui_small, 410, 318, "Rumble", C_DIM);
        ui_button(410, 330, 116, 40, "Light", C_RAISED, ID_WEAK);
        ui_button(532, 330, 116, 40, "Heavy", C_RAISED, ID_STRONG);
        ui_button(654, 330, 116, 40, "Both", C_BLUE, ID_BOTH);
    }
    if (s_note[0]) ui_text_r(ui_small, 780, 400, s_note, C_ORANGE);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    papp_surface_t *s = ui_open();
    if (!s) return 1;
    for (int i = 0; i < PAPP_MAX_PLAYERS; i++) {     /* start on the first connected player */
        papp_gamepad_t g;
        if (papp_input_gamepad(i, &g)) { s_tab = i; break; }
    }
    while (!papp_exit_requested() && !s_quit) {
        ui_touch(s, act);
        ui_begin(s);
        papp_joystick_t j;
        char status[96];
        uint16_t dot = C_FAINT;
        if (papp_input_joystick(s_tab, &j)) {
            snprintf(status, sizeof(status), "%s  -  %s  %04X:%04X%s%s", j.name,
                     j.kind < sizeof(k_kind) / sizeof(k_kind[0]) ? k_kind[j.kind] : "Controller", j.vid, j.pid,
                     j.flags & PAPP_JOY_JOINED ? "  + throttle" : "", j.flags & PAPP_JOY_MOTION ? "  motion" : "");
            dot = C_GREEN;
        } else {
            snprintf(status, sizeof(status), "Player %d: not connected", s_tab + 1);
        }
        ui_bar("Controller test", NULL, 0, s_tab, status, dot, false);
        for (int i = 0; i < PAPP_MAX_PLAYERS; i++) {            /* 4 players: narrower tabs than ui_bar's */
            papp_gamepad_t g;
            bool on = s_tab == i, here = papp_input_gamepad(i, &g);
            int x = 290 + i * 108;
            char t[4] = { 'P', (char)('1' + i), 0 };
            ui_rrect(x, 12, 96, 40, 20, ui_pressed_color(UI_ID_TAB + i, on ? C_BLUE : C_RAISED));
            ui_text_c(ui_med, x + 48, 32, t, on ? C_TEXT : here ? C_GREEN : C_DIM);
            ui_hit(x, 4, 96, 56, UI_ID_TAB + i, UI_TAP);
        }
        draw_player(s_tab);
        ui_end(s);
    }
    papp_input_rumble(s_tab, 0, 0, 0);
    papp_disp_surface_destroy(s);
    return 0;
}
