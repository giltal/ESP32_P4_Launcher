/*
 * input_test — INPT interface test.
 *   args: [seconds] (default 15)
 * Logs every input event (device added/removed, button and key down/up with
 * text) and, once a second while a pad moves, player 1's sticks and triggers.
 * The paddle (if the board has one) is logged when it moves by >1/32.
 * Typed text is echoed as one line at the end. Exit code = events seen.
 */
#include "papp/papp.h"
#include <stdlib.h>

PAPP_MANIFEST(.id = "test.input", .name = "Input test", .version = "0.1.0", .main_stack = 8192);

static const char *const s_btn[20] = {
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "SELECT", "START", "L3", "R3", "HOME", "MISC",
    "?", "?", "UP", "DOWN", "LEFT", "RIGHT",
};

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 15;
    papp_log("input test for %d s\n", seconds);
    for (int i = 0; i < PAPP_MAX_PLAYERS; i++) {
        papp_gamepad_t g;
        if (papp_input_gamepad(i, &g)) papp_log("P%d: %s (%04X:%04X)\n", i + 1, g.name, g.vid, g.pid);
    }

    char typed[80];
    int ntyped = 0, events = 0;
    int64_t t0 = papp_time_us(), last_axes = 0;
    papp_gamepad_t prev = { 0 };
    int paddle = papp_input_analog(PAPP_ANALOG_PADDLE);
    if (paddle >= 0) papp_log("paddle %d\n", paddle);
    while (!papp_exit_requested() && papp_time_us() - t0 < (int64_t)seconds * 1000000) {
        papp_input_event_t ev;
        while (papp_input_poll_event(&ev)) {
            events++;
            switch (ev.type) {
            case PAPP_EV_BUTTON_DOWN:
            case PAPP_EV_BUTTON_UP:
                papp_log("P%d %s %s\n", ev.device + 1, ev.code < 20 ? s_btn[ev.code] : "?",
                         ev.type == PAPP_EV_BUTTON_DOWN ? "down" : "up");
                break;
            case PAPP_EV_KEY_DOWN:
                papp_log("key 0x%02X down mods %02X text '%c'\n", ev.code, ev.mods,
                         ev.text >= 32 && ev.text < 127 ? ev.text : ' ');
                if (ev.text >= 32 && ev.text < 127 && ntyped < (int)sizeof(typed) - 1) typed[ntyped++] = ev.text;
                break;
            case PAPP_EV_KEY_UP:
                break;
            case PAPP_EV_DEVICE_ADDED:
            case PAPP_EV_DEVICE_REMOVED:
                papp_log("%s %s\n", ev.device == 0xFF ? "keyboard" : "gamepad",
                         ev.type == PAPP_EV_DEVICE_ADDED ? "added" : "removed");
                break;
            }
        }

        papp_gamepad_t g;
        if (papp_input_gamepad(0, &g) && papp_time_us() - last_axes > 1000000) {
            int moved = abs(g.lx - prev.lx) + abs(g.ly - prev.ly) + abs(g.rx - prev.rx) +
                        abs(g.ry - prev.ry) + abs(g.lt - prev.lt) + abs(g.rt - prev.rt);
            if (moved > 4000) {
                papp_log("P1 sticks L(%d,%d) R(%d,%d) triggers %u %u\n", g.lx, g.ly, g.rx, g.ry, g.lt, g.rt);
                prev = g;
                last_axes = papp_time_us();
            }
        }
        int pv = papp_input_analog(PAPP_ANALOG_PADDLE);
        if (pv >= 0 && abs(pv - paddle) > 2048) {
            papp_log("paddle %d\n", pv);
            paddle = pv;
        }
        papp_delay_ms(10);
    }
    typed[ntyped] = 0;
    papp_log("%d events, typed \"%s\"\n", events, typed);
    return events;
}
