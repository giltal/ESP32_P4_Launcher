/*
 * Input manager (ARCHITECTURE §7.5, first cut).
 *
 * Sources (USB HID gamepads/keyboards, board buttons, DevLink injection)
 * feed logical devices: up to PAPP_MAX_PLAYERS gamepads in the standard
 * layout and one merged keyboard. Changes become events in one queue that the
 * running app drains through INPT. HOME (or Select+Start held 1 s) is a system
 * chord and goes to the launcher. Board buttons (BSP) merge into player 1.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "papp_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LX_INPUT_SYS_HOME = 1,      /* HOME pressed, or Select+Start held 1 s */
} lx_input_sys_t;

typedef void (*lx_input_sys_cb_t)(lx_input_sys_t ev);

/* Starts the USB HID host when the board has BSP_CAP_USB_HOST. */
esp_err_t lx_input_init(lx_input_sys_cb_t sys_cb);

/* Consumers */
bool     lx_input_gamepad(int player, papp_gamepad_t *out);
bool     lx_input_key_down(uint16_t usage);
bool     lx_input_keyboard_attached(void);          /* a USB keyboard or the board's own */
uint16_t lx_input_modifiers(void);
bool     lx_input_poll_event(papp_input_event_t *out);
void     lx_input_flush(void);
/* One-line summary of connected devices, for launcher screens. */
void     lx_input_describe(char *buf, size_t len);
/* PAPP_ANALOG_*: 0..65535, -1 if the board has no such input. */
int      lx_input_analog(int ch);
/* INPT v4 */
bool     lx_input_joystick(int player, papp_joystick_t *out);
bool     lx_input_motion(int player, papp_motion_t *out);
bool     lx_input_rumble(int player, uint16_t strong, uint16_t weak, uint32_t ms);  /* false: no motors */
void     lx_input_rumble_stop(void);                /* every motor off, wheels back to the user's settings (app teardown) */
/* INPT v5, force-feedback wheels; false: not one */
bool     lx_input_wheel_range(int player, uint16_t degrees);   /* 0 = the user's */
bool     lx_input_force(int player, int16_t force);
bool     lx_input_spring(int player, int32_t strength);        /* -1 = the user's */
/* The user's settings for the player's wheel (gamepads_mine.txt range= / spring=): false = no FFB wheel there */
bool     lx_input_wheel_settings(int player, uint16_t *range, int *spring_pct);
esp_err_t lx_input_wheel_set_settings(int player, uint16_t range, int spring_pct);   /* saved; task with an internal stack */

/* Sources: gamepads */
int  lx_input_pad_attach(const char *name, uint16_t vid, uint16_t pid);   /* -> player or -1 */
void lx_input_pad_update(int player, uint32_t buttons, int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                         uint16_t lt, uint16_t rt);
void lx_input_pad_detach(int player);
/* kind = PAPP_PAD_*. A throttle joins the player of a flight stick that has none: the handle is then
 * player + PAPP_MAX_PLAYERS, used like a player with update / joystick / detach */
int  lx_input_pad_attach_kind(const char *name, uint16_t vid, uint16_t pid, int kind);
void lx_input_pad_set_kind(int player, int kind);    /* it changed (a Wii Remote got a Nunchuk) */
/* INPT v4 views, after lx_input_pad_update: every control (sources that know more than the standard
 * pad), motion sensors */
void lx_input_pad_joystick(int player, const papp_joystick_t *j);
void lx_input_pad_motion(int player, const papp_motion_t *m);
/* Outputs: the source's function sends rumble and lights to the pad. Runs on the "lx_padout" task (internal
 * stack), only for changes, may block on the transport. Set it after attach; NULL before freeing ctx (waits
 * for a send in progress). Setting one gives a short buzz and the player's colour */
typedef struct {
    uint16_t strong, weak;      /* motors 0..65535: heavy (left), light (right) */
    uint8_t  r, g, b;           /* light bar: the player's colour */
    uint8_t  player;            /* 0..3: player LEDs */
    /* force-feedback wheels (lx_input_pad_set_ffb): */
    int16_t  force;             /* steady push, -32768 left .. 32767 right; a rumble adds the shake */
    uint16_t spring;            /* centring spring 0..65535 */
    uint16_t range;             /* degrees lock to lock */
    uint8_t  shake;             /* rumble on a wheel: 0 none, 1 / 2 = push one way / the other (strong, weak: how
                                   hard); the task flips it every LX_PAD_SHAKE_MS */
} lx_pad_out_state_t;
#define LX_PAD_SHAKE_MS 30
#define LX_PAD_RANGE_DEFAULT  900
#define LX_PAD_SPRING_DEFAULT 25    /* percent */
typedef void (*lx_pad_out_fn)(void *ctx, const lx_pad_out_state_t *s);
void lx_input_pad_set_out(int player, lx_pad_out_fn fn, void *ctx);
/* The output function also drives a force-feedback wheel (force, spring, range, shake); after set_out */
void lx_input_pad_set_ffb(int player, bool on);

/* Sources: HID gamepads of other transports (Bluetooth, lx_input_hidpad.c). open attaches a
 * player (NULL: no free slot); desc = the HID report descriptor (parsed here, may be freed after);
 * report = one input report, report id first when the pad uses ids. */
typedef struct lx_hidpad lx_hidpad_t;
lx_hidpad_t *lx_input_hidpad_open(const char *name, uint16_t vid, uint16_t pid, const uint8_t *desc, size_t len);
void         lx_input_hidpad_report(lx_hidpad_t *pad, const uint8_t *report, size_t len);
void         lx_input_hidpad_close(lx_hidpad_t *pad);
/* How to send output reports (rumble, lights) to the pad: data = the report, id first. Called on the
 * lx_padout task. NULL: none any more (call it before the transport's context goes) */
typedef void (*lx_hidpad_send_fn)(void *ctx, const uint8_t *data, size_t len);
void         lx_input_hidpad_set_sender(lx_hidpad_t *pad, lx_hidpad_send_fn fn, void *ctx);
void        *lx_input_hidpad_sender_ctx(lx_hidpad_t *pad);   /* to free it after close */
/* Report timing since the last call (then reset): a stall is a gap of LX_HIDPAD_STALL_US or more */
#define LX_HIDPAD_STALL_US 100000
typedef struct {
    int      player;
    uint32_t reports, ms;       /* reports in the last ms milliseconds */
    uint32_t gap_max_ms;        /* longest gap between two reports */
    uint32_t stalls;
    uint32_t stalls_in_use;     /* stalls within LX_HIDPAD_IN_USE_US of an input change (someone playing) */
    uint32_t idle_ms;           /* since the last report */
    uint32_t still_ms;          /* since the last input change (buttons, or sticks / triggers moved) */
} lx_hidpad_stats_t;
#define LX_HIDPAD_IN_USE_US 2000000
void         lx_input_hidpad_stats(lx_hidpad_t *pad, lx_hidpad_stats_t *out);
uint32_t     lx_input_hidpad_still_ms(lx_hidpad_t *pad);   /* since the last input change; no reset */

/* Button mapping, per pad model (VID:PID), as text on the SD card: your own in
 * /sd/config/gamepads_mine.txt, else the shared /sd/config/gamepads.txt (lx_padmap.c).
 * Bits are PAPP_BTN_ positions. set_map also clears a pre-0.23 copy in NVS (flash):
 * call it from a task with an internal stack, never from an app thread. */
#define LX_INPUT_MAP_BITS 20
int       lx_input_pad_first(void);                 /* first real pad, or -1 */
/* Its picture's name in /sd/config/pads (<name>.png + <name>.txt, tools/make_pad_pictures.py): gamepads.txt's
 * pic=, else by kind and maker ("gamepad" for any other). false = no pad there */
bool      lx_input_pad_picture(int player, char *name, size_t len);
uint32_t  lx_input_pad_raw(int player);             /* buttons before the mapping */
esp_err_t lx_input_pad_set_map(int player, const uint8_t *map);   /* NULL: remove yours (database or default) */
void      lx_input_pad_reload_maps(void);           /* the files changed: apply them to the connected pads */

/* Sources: keyboards (boot report: modifiers + up to 6 usages) */
void lx_input_kbd_attach(void);
void lx_input_kbd_report(uint8_t mods, const uint8_t keys[6]);
void lx_input_kbd_detach(void);

/* DevLink injection: press for hold_ms, then release */
void lx_input_inject_buttons(int player, uint32_t buttons, uint32_t hold_ms);
void lx_input_inject_key(uint16_t usage, uint16_t mods, uint32_t hold_ms);

#ifdef __cplusplus
}
#endif
