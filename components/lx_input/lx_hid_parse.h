/*
 * HID report descriptor parser: finds a gamepad's, joystick's or wheel's axes, hats and buttons so its
 * reports can be read whatever their layout. Plain C, no ESP-IDF, so a test app can build it too
 * (apps/hid_test).
 *
 * Axes are kept by role (HID_AX_*): the Generic Desktop ones (X Y Z Rx Ry Rz Slider Dial Wheel) and the
 * Simulation page's (Throttle Rudder Steering Accelerator Brake). Up to 32 buttons and 2 hats. A device
 * may spread them over several reports (report ids): each field remembers its report, and decoding
 * merges one report into a state the caller keeps.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    HID_AX_X, HID_AX_Y, HID_AX_Z, HID_AX_RX, HID_AX_RY, HID_AX_RZ,     /* Generic Desktop 0x30..0x35 */
    HID_AX_SLIDER, HID_AX_DIAL, HID_AX_WHEEL,                          /* 0x36..0x38 */
    HID_AX_THROTTLE, HID_AX_RUDDER, HID_AX_STEERING, HID_AX_ACCEL, HID_AX_BRAKE,   /* Simulation page */
    HID_AX_SLIDER2,                                                    /* a second Slider */
    HID_AX_COUNT
};

#define HID_MAX_BUTTONS 32
#define HID_MAX_HATS    2
#define HID_MAX_IDS     4

/* What the top-level collection says the device is */
enum { HID_DEV_GAMEPAD, HID_DEV_JOYSTICK, HID_DEV_MULTIAXIS, HID_DEV_SIMULATION };

typedef struct {
    uint16_t bit;           /* offset in the report, after the report id */
    uint8_t  size;          /* bits; 0 = absent */
    uint8_t  id;            /* report id it arrives in (0: the device doesn't use ids) */
    bool     sign;
    int32_t  min, max;
} hid_field_t;

typedef struct {
    uint8_t     report_id;  /* the first report with controls (0: no ids) */
    uint8_t     ids[HID_MAX_IDS], nids;   /* every report that carries controls */
    uint8_t     device;     /* HID_DEV_* */
    hid_field_t axis[HID_AX_COUNT];
    hid_field_t hat[HID_MAX_HATS];
    uint8_t     hats;
    hid_field_t button[HID_MAX_BUTTONS];  /* button n + 1; size 0 = absent */
    uint8_t     buttons;    /* highest button number present */
} hid_layout_t;

/* False when the descriptor doesn't describe a usable gamepad, joystick or wheel. */
bool hid_layout_parse(const uint8_t *desc, size_t len, hid_layout_t *out);

typedef struct {
    int16_t  axis[HID_AX_COUNT];    /* -32768..32767, 0 if absent */
    int8_t   hat[HID_MAX_HATS];     /* 0..7 clockwise from up, -1 centred or absent */
    uint32_t buttons;               /* bit n = HID button n + 1 */
} hid_state_t;

/* Before the first decode: everything centred and released */
void hid_state_init(hid_state_t *s);

/* Merges one input report into *s (fields of other reports keep their values). False when the
 * report carries none of the layout's controls. */
bool hid_layout_decode(const hid_layout_t *l, const uint8_t *report, size_t len, hid_state_t *s);

/* The axis is present */
static inline bool hid_has(const hid_layout_t *l, int ax) { return l->axis[ax].size != 0; }
