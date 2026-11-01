/*
 * HID report descriptor parser: finds a gamepad's axes, hat and buttons so its
 * reports can be read whatever their layout. Plain C, no ESP-IDF, so a test app
 * can build it too (apps/hid_test).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { HID_AX_X, HID_AX_Y, HID_AX_Z, HID_AX_RX, HID_AX_RY, HID_AX_RZ, HID_AX_COUNT };

#define HID_MAX_BUTTONS 16

typedef struct {
    uint16_t bit;           /* offset in the report, after the report id */
    uint8_t  size;          /* bits; 0 = absent */
    bool     sign;
    int32_t  min, max;
} hid_field_t;

typedef struct {
    uint8_t     report_id;  /* 0: the device doesn't use report ids */
    hid_field_t axis[HID_AX_COUNT];
    hid_field_t hat;
    uint16_t    button_bit;
    uint8_t     buttons;
} hid_layout_t;

/* False when the descriptor doesn't describe a usable gamepad. */
bool hid_layout_parse(const uint8_t *desc, size_t len, hid_layout_t *out);

typedef struct {
    int16_t  axis[HID_AX_COUNT];    /* -32768..32767, 0 if absent */
    int8_t   hat;                   /* 0..7 clockwise from up, -1 centred or absent */
    uint16_t buttons;               /* bit n = HID button n + 1 */
} hid_state_t;

/* False when the report isn't the one the layout describes. */
bool hid_layout_decode(const hid_layout_t *l, const uint8_t *report, size_t len, hid_state_t *out);
