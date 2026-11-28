#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t lx_input_usb_start(void);
esp_err_t lx_input_xinput_start(void);     /* after usb_start: the USB host is installed */
/* A USB device opened by the XInput client (for another driver's device: its handle for transfers on
 * endpoints that driver claimed). NULL: not possible. Close it before the device is gone for good */
#include <stdint.h>
struct usb_device_handle_s;                 /* = usb_device_handle_t (usb_host.h stays out of this header) */
struct usb_device_handle_s *lx_input_usb_dev_open(uint8_t addr);
void lx_input_usb_dev_close(struct usb_device_handle_s *dev);       /* NULL: nothing */

/* Button mappings as text on the SD card (lx_padmap.c) */
#include <stdint.h>
#define LX_PADMAP_DIR "/sd/config"
enum { LX_PADMAP_NONE, LX_PADMAP_MINE, LX_PADMAP_DB };
int       lx_padmap_load(uint16_t vid, uint16_t pid, uint8_t *map);     /* -> LX_PADMAP_ */
esp_err_t lx_padmap_save(uint16_t vid, uint16_t pid, const char *name, const uint8_t *map);  /* NULL: remove */
/* range= / spring= on your line (made from the database's when you have none), the buttons kept */
esp_err_t lx_padmap_save_wheel(uint16_t vid, uint16_t pid, const char *name, uint16_t range, int spring_pct);
/* The 0.38 additions of a pad's line: kind, axis remaps, picture (gamepads_mine.txt's line wins whole) */
#define LX_PADMAP_AXES 8
enum { LX_PADAX_LX, LX_PADAX_LY, LX_PADAX_RX, LX_PADAX_RY, LX_PADAX_LT, LX_PADAX_RT };
typedef struct {
    int8_t  kind;               /* PAPP_PAD_*, -1 = guessed from the axes */
    uint8_t naxis;
    struct { uint8_t role, to; bool inv; } axis[LX_PADMAP_AXES];   /* HID_AX_ -> LX_PADAX_ (0xFF = none) */
    char    pic[24];
    uint16_t range;             /* range=<degrees>: a force-feedback wheel's rotation, 0 = not given */
    bool    has_spring;
    uint8_t spring;             /* spring=<percent> 0..100: its centring spring */
} lx_padmap_ext_t;
bool      lx_padmap_ext(uint16_t vid, uint16_t pid, lx_padmap_ext_t *ext);   /* false: no line */

/* Gamepad report decoding (lx_pad_decode.c), shared by USB and Bluetooth pads */
#include <stdbool.h>
#include <stddef.h>
#include "lx_hid_parse.h"
typedef struct {
    uint32_t b;
    int16_t  lx, ly, rx, ry;
    uint16_t lt, rt;
} lx_pad_state_t;
uint32_t lx_pad_hat(uint8_t hat);                   /* 0..7 clockwise from up -> PAPP_BTN_ d-pad */
uint32_t lx_pad_axes_dpad(uint8_t x, uint8_t y, uint8_t lo, uint8_t hi);
uint32_t lx_pad_ps_buttons(uint8_t hat_btn, uint8_t b2, uint8_t b3);
void     lx_pad_ps4(const uint8_t *p, lx_pad_state_t *s);   /* DS4 report 0x01 layout, p[0] = id, 10 bytes */
void     lx_pad_ps5(const uint8_t *p, lx_pad_state_t *s);   /* DualSense USB report layout, 11 bytes */
#include "papp_abi.h"
int      lx_pad_kind(const hid_layout_t *l);        /* PAPP_PAD_* the layout looks like */
/* Merges report p into *raw (the device's state, hid_state_init first), then the standard pad view */
bool     lx_pad_layout(const hid_layout_t *l, int kind, hid_state_t *raw, const uint8_t *p, size_t len,
                       lx_pad_state_t *out);
void     lx_pad_remap(const lx_padmap_ext_t *e, const hid_layout_t *l, const hid_state_t *s, lx_pad_state_t *out);
/* the player's kind (gamepads.txt's kind= applied at attach) and line additions; -1 = no such player */
int      lx_input_pad_ext(int player, lx_padmap_ext_t *ext);
uint32_t lx_input_pad_ext_gen(int player);          /* changes when the mapping files were read again */
void     lx_pad_std(const hid_layout_t *l, int kind, const hid_state_t *s, lx_pad_state_t *out);
/* INPT v4 joystick view: kind, axes, hats, buttons (not connected, vid, pid, name, flags) */
void     lx_pad_joystick(const hid_layout_t *l, int kind, const hid_state_t *s, papp_joystick_t *j);
void     lx_pad_joystick_std(const lx_pad_state_t *s, int kind, papp_joystick_t *j);   /* pads read by model */
uint8_t  lx_pad_ps5_leds(int player);                /* DualSense player LED pattern */

/* Wii Remote and its extensions, Wii U Pro (lx_pad_wii.c), for lx_input_hidpad.c */
#include "lx_input.h"
typedef struct {
    void   (*send)(void *ctx, const uint8_t *data, size_t len);
    void    *ctx;
    uint8_t  ext, st, battery, leds;
    bool     rumble, pro, kind_changed;
} lx_wii_t;
void lx_wii_start(lx_wii_t *w, void (*fn)(void *ctx, const uint8_t *data, size_t len), void *ctx, bool pro);
void lx_wii_out(lx_wii_t *w, const lx_pad_out_state_t *o);
int  lx_wii_kind(const lx_wii_t *w);               /* PAPP_PAD_* for what is plugged in */
/* One input report: false if it isn't a data report. Also answers the set-up reports (status, acks) */
bool lx_wii_report(lx_wii_t *w, const uint8_t *p, size_t len, lx_pad_state_t *s, papp_motion_t *m, bool *has_m);
