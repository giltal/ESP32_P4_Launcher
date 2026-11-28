/*
 * USB HID host source for the input manager: gamepads and boot keyboards.
 * Report parsers ported from RetroESP32_P4 components/gamepad (PS3, PS4, PS5,
 * generic, Dual PSX adaptor), extended to several devices at once.
 *
 * Tasks: usb_host (library events), HID host driver task (callbacks), and
 * lx_hid (opens devices — needs delays, so not in the callback).
 */
#include "lx_input.h"
#include "lx_input_priv.h"
#include "lx_hid_parse.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "hal/usb_dwc_hal.h"
#include "hal/usb_dwc_ll.h"
#include "soc/usb_utmi_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "lx_hid";

/* ── USB host BNA workaround ────────────────────────────────────────────────
 * IDF 5.5 (v5.5.2 to v5.5.6) usb_dwc_hal_chan_decode_intr() asserts that every channel error
 * comes with CHHLTD. BNA (the DMA fetched a descriptor that wasn't ready, seen
 * when apps load both cores and the PPA) arrives without it, so the assert
 * panics in the USB interrupt. Wrapped at link time (CMakeLists.txt): an error
 * without a halt is reported as a transfer error, at once if the channel has
 * already stopped (every BNA seen so far: disabling it would raise no CHHLTD),
 * else on the CHHLTD that disabling it raises. The URB fails and interface_cb
 * restarts the device; the pad is back within ~50 ms. */

#define CHAN_ERR_MSK (USB_DWC_LL_INTR_CHAN_STALL | USB_DWC_LL_INTR_CHAN_BBLEER | \
                      USB_DWC_LL_INTR_CHAN_BNAINTR | USB_DWC_LL_INTR_CHAN_XCS_XACT_ERR)

static uint32_t s_chan_err[16];             /* deferred error bits per channel */
static volatile uint32_t s_bna_count;       /* channel errors without a halt (logged by lx_hid) */

usb_dwc_hal_chan_event_t __real_usb_dwc_hal_chan_decode_intr(usb_dwc_hal_chan_t *chan);

/* Hubs: IDF has no transaction translator, so a low/full-speed pad behind a high-speed hub is
 * refused. With the board's port limited to full speed the hub runs at full speed too and just
 * repeats the pads' traffic (pads and keyboards are full/low speed anyway). The bit goes in every
 * time IDF powers the port: its recovery after a disconnect soft-resets the controller, which
 * clears it. hcd_port_command and its POWER_ON (0) are IDF's private hcd.h. */
#if CONFIG_USB_HOST_HUBS_SUPPORTED
/* Full speed on the HS core's UTMI PHY: 30 MHz PHY clock (select 0, 30000 clocks a frame) and the
 * PHY's FS end-of-packet detect + low-speed preamble handling, without which a low-speed pad behind
 * the hub never answers its first request (found by NonaSuomy, PR #23). */
static void usb_fs_only(void)
{
    usb_dwc_dev_t *hw = &USB_DWC_HS;
    usb_dwc_ll_hcfg_set_fsls_supp_only(hw);
    hw->hcfg_reg.fslspclksel = 0;
    usb_dwc_hfir_reg_t hfir = { .val = hw->hfir_reg.val };
    hfir.hfirrldctrl = 0;
    hfir.frint = 29999;
    hw->hfir_reg.val = hfir.val;
    USB_UTMI.fc_06.det_fseop_en = 1;
    USB_UTMI.fc_06.pre_hphy_lsie = 1;
}
#endif

esp_err_t __real_hcd_port_command(void *port, int command);
esp_err_t __wrap_hcd_port_command(void *port, int command)
{
#if CONFIG_USB_HOST_HUBS_SUPPORTED
    if (command == 0) usb_fs_only();                /* POWER_ON */
    esp_err_t e = __real_hcd_port_command(port, command);
    if (command == 2) usb_fs_only();                /* RESET: the enable may pick other values */
    return e;
#else
    return __real_hcd_port_command(port, command);
#endif
}

/* The channel has stopped with error bits `e`: report it as IDF's error path would */
static IRAM_ATTR usb_dwc_hal_chan_event_t chan_stopped_with_error(usb_dwc_hal_chan_t *chan, uint32_t e)
{
    chan->flags.active = 0;
    if (chan->flags.halt_requested) {
        /* The HCD asked for this halt meanwhile and waits for HALT_REQ;
           it treats a halted transfer as failed anyway */
        chan->flags.halt_requested = 0;
        return USB_DWC_HAL_CHAN_EVENT_HALT_REQ;
    }
    /* Not USB_DWC_HAL_CHAN_ERROR_BNA: the HCD maps it to ERROR_URB_NOT_AVAIL,
       which _buffer_parse_error() aborts on. A transfer error fails the URB. */
    chan->error = (e & USB_DWC_LL_INTR_CHAN_STALL)  ? USB_DWC_HAL_CHAN_ERROR_STALL :
                  (e & USB_DWC_LL_INTR_CHAN_BBLEER) ? USB_DWC_HAL_CHAN_ERROR_PKT_BBL :
                                                      USB_DWC_HAL_CHAN_ERROR_XCS_XACT;
    return USB_DWC_HAL_CHAN_EVENT_ERROR;
}

IRAM_ATTR usb_dwc_hal_chan_event_t __wrap_usb_dwc_hal_chan_decode_intr(usb_dwc_hal_chan_t *chan)
{
    volatile usb_dwc_host_chan_regs_t *r = chan->regs;
    uint32_t in = r->hcint_reg.val;
    int i = chan->flags.chan_idx;

    if ((in & CHAN_ERR_MSK) && !(in & USB_DWC_LL_INTR_CHAN_CHHLTD)) {
        r->hcint_reg.val = in & CHAN_ERR_MSK;           /* write 1 to clear: only the errors */
        s_bna_count++;
        if (!usb_dwc_ll_hcchar_chan_is_enabled(r)) {
            /* Already stopped (as after a BNA): disabling it raises no CHHLTD */
            uint32_t e = s_chan_err[i] | (in & CHAN_ERR_MSK);
            s_chan_err[i] = 0;
            return chan_stopped_with_error(chan, e);
        }
        s_chan_err[i] |= in & CHAN_ERR_MSK;
        usb_dwc_ll_hcchar_disable_chan(r);              /* CHHLTD follows */
        return USB_DWC_HAL_CHAN_EVENT_NONE;
    }
    if (s_chan_err[i] && (in & USB_DWC_LL_INTR_CHAN_CHHLTD)) {
        uint32_t e = s_chan_err[i] | (in & CHAN_ERR_MSK);
        s_chan_err[i] = 0;
        r->hcint_reg.val = in;
        return chan_stopped_with_error(chan, e);
    }
    return __real_usb_dwc_hal_chan_decode_intr(chan);
}

#define MAX_DEVS 8

typedef enum { DEV_FREE = 0, DEV_PAD, DEV_KBD, DEV_IGNORED } dev_kind_t;

typedef enum {
    FMT_UNKNOWN = 0, FMT_PS3, FMT_PS4, FMT_PS5, FMT_GENERIC, FMT_PSX_ADAPTOR, FMT_HID,
} pad_fmt_t;

typedef struct {
    hid_host_device_handle_t handle;
    dev_kind_t kind;
    int player;
    pad_fmt_t fmt;
    int detect_count;
    uint16_t vid, pid;
    bool waking;                    /* DS3 asleep: its wake-up press isn't HOME */
    bool has_layout;
    hid_layout_t layout;            /* from the report descriptor */
    hid_state_t raw;                /* its controls, kept across reports (they may come in several) */
    int pkind;                      /* PAPP_PAD_* */
    lx_padmap_ext_t ext;            /* gamepads.txt: axis remaps */
    uint32_t ext_gen;
    /* Output reports on the interrupt OUT endpoint (the DS4 ignores SET_REPORT rumble): the device opened
       by the XInput client, as the HID driver keeps its handle to itself */
    usb_device_handle_t out_dev;
    uint8_t out_ep;
    bool out_looked;
    int32_t lg_range, lg_spring, lg_const;     /* Logitech force feedback: last sent, -1 = not yet */
} dev_t;

/* PSRAM: 7 KB with the joystick state; internal RAM is short at start-up on v1.x chips (0.38 test build:
 * 8 KB more there and the FreeRTOS timer task found no stack) */
static EXT_RAM_BSS_ATTR dev_t s_devs[MAX_DEVS];
static QueueHandle_t s_connect_q;
static QueueHandle_t s_restart_q;          /* devices whose transfer failed */

static dev_t *find_dev(hid_host_device_handle_t h)
{
    for (int i = 0; i < MAX_DEVS; i++)
        if (s_devs[i].kind != DEV_FREE && s_devs[i].handle == h) return &s_devs[i];
    return NULL;
}

/* ── Gamepad report parsing ─────────────────────────────────────────────── */

#define hat_to_dpad  lx_pad_hat
#define axes_to_dpad lx_pad_axes_dpad
#define ps_buttons   lx_pad_ps_buttons

static bool centred(uint8_t v, uint8_t lo, uint8_t hi) { return v >= lo && v <= hi; }

static pad_fmt_t detect_format(const dev_t *d, const uint8_t *p, int len)
{
    if (len == 8 && d->vid == 0x0810 && d->pid == 0x0001) return FMT_PSX_ADAPTOR;
    if (d->vid == 0x054C && d->pid == 0x0268 && len >= 49) return FMT_PS3;
    if (d->has_layout && d->vid != 0x054C) return FMT_HID;  /* the pad says what it sends */
    if (len < 10 || p[0] != 0x01) return FMT_GENERIC;
    if (len >= 49) {
        bool ps3_axes = centred(p[6], 0x60, 0xA0) && centred(p[7], 0x60, 0xA0) &&
                        centred(p[8], 0x60, 0xA0) && centred(p[9], 0x60, 0xA0);
        bool ps4_axes = centred(p[1], 0x70, 0x90) && centred(p[2], 0x70, 0x90);
        if (ps3_axes && !ps4_axes && p[1] == 0x00) return FMT_PS3;
    }
    if (centred(p[1], 0x70, 0x90) && centred(p[2], 0x70, 0x90) &&
        centred(p[3], 0x70, 0x90) && centred(p[4], 0x70, 0x90)) {
        if (p[5] == 0x08 && p[8] != 0x08) return FMT_PS4;
        if (p[5] == 0x00 && p[8] == 0x08) return FMT_PS5;
    }
    uint8_t h5 = p[5] & 0x0F, h8 = p[8] & 0x0F;
    if (h5 <= 8 && h8 > 8) return FMT_PS4;
    if (h8 <= 8 && h5 > 8) return FMT_PS5;
    return FMT_PS4;
}

#define AX(v)  ((int16_t)(((int)(v) - 128) * 256))
#define TRIG(v) ((uint16_t)((v) * 257))

static void parse_hid(dev_t *d, const uint8_t *p, int len)
{
    lx_pad_state_t s;
    if (!lx_pad_layout(&d->layout, d->pkind, &d->raw, p, (size_t)len, &s)) return;
    uint32_t gen = lx_input_pad_ext_gen(d->player);
    if (gen != d->ext_gen) {                /* the mapping files changed: their axis lines again */
        d->ext_gen = gen;
        lx_input_pad_ext(d->player, &d->ext);
    }
    lx_pad_remap(&d->ext, &d->layout, &d->raw, &s);
    lx_input_pad_update(d->player, s.b, s.lx, s.ly, s.rx, s.ry, s.lt, s.rt);
    papp_joystick_t j = { 0 };
    lx_pad_joystick(&d->layout, d->pkind, &d->raw, &j);
    lx_input_pad_joystick(d->player, &j);
}

/* DualShock 4 (report 0x01, 64 bytes) and DualSense (0x01): gyro + accelerometer */
static void sony_motion(dev_t *d, const uint8_t *p, int len)
{
    int at = d->fmt == FMT_PS4 ? 13 : 16;
    if (len < at + 12 || p[0] != 0x01) return;
    papp_motion_t m = { .valid = PAPP_MOTION_ACCEL | PAPP_MOTION_GYRO, .ir_x = 0xFFFF, .ir_y = 0xFFFF,
                        .time_ms = (uint32_t)(esp_timer_get_time() / 1000) };
    for (int i = 0; i < 3; i++) {
        m.gyro[i] = (int16_t)(p[at + 2 * i] | p[at + 2 * i + 1] << 8);
        m.accel[i] = (int16_t)((int16_t)(p[at + 6 + 2 * i] | p[at + 7 + 2 * i] << 8) / 2);  /* 8192 per g */
    }
    lx_input_pad_motion(d->player, &m);
}

static void out_done(usb_transfer_t *t)
{
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) ESP_LOGD(TAG, "output report: status %d", t->status);
    usb_host_transfer_free(t);
}

/* The HID interface's interrupt OUT endpoint, first call only; 0 = none (SET_REPORT then) */
static void find_out_ep(dev_t *d)
{
    d->out_looked = true;
    hid_host_dev_params_t params;
    const usb_config_desc_t *cfg;
    if (hid_host_device_get_params(d->handle, &params) != ESP_OK) return;
    d->out_dev = lx_input_usb_dev_open(params.addr);
    if (!d->out_dev || usb_host_get_active_config_descriptor(d->out_dev, &cfg) != ESP_OK) return;
    const uint8_t *p = (const uint8_t *)cfg, *end = p + cfg->wTotalLength;
    bool ours = false;
    for (; p + 2 <= end && p[0]; p += p[0]) {
        if (p[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)p;
            ours = i->bInterfaceNumber == params.iface_num && i->bAlternateSetting == 0;
        } else if (ours && p[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)p;
            if ((e->bmAttributes & 0x03) == USB_TRANSFER_TYPE_INTR && !(e->bEndpointAddress & 0x80)) {
                d->out_ep = e->bEndpointAddress;
                break;
            }
        }
    }
    ESP_LOGI(TAG, "player %d: output reports on %s", d->player + 1, d->out_ep ? "the interrupt OUT endpoint" : "SET_REPORT");
}

/* An output report: interrupt OUT, as Linux and SDL send them, else SET_REPORT. r = the report as sent (with the
 * id first when the device uses ids: id = r[0]; 0 = no ids) */
static esp_err_t send_output(dev_t *d, uint8_t id, const uint8_t *r, size_t len)
{
    if (!d->out_looked) find_out_ep(d);
    usb_transfer_t *t;
    if (d->out_ep && usb_host_transfer_alloc(64, 0, &t) == ESP_OK) {
        memcpy(t->data_buffer, r, len);
        t->num_bytes = len;
        t->device_handle = d->out_dev;
        t->bEndpointAddress = d->out_ep;
        t->callback = out_done;
        t->context = NULL;
        esp_err_t e = usb_host_transfer_submit(t);
        if (e == ESP_OK) return ESP_OK;
        usb_host_transfer_free(t);
        return e;
    }
    return hid_class_request_set_report(d->handle, HID_REPORT_TYPE_OUTPUT, id, (uint8_t *)r, len);
}

/* ── Logitech wheels (commands as in Linux's hid-lg4ff: 7-byte output reports, no report id) ── */

static bool logitech_wheel(uint16_t vid, uint16_t pid)
{
    /* Driving Force / EX (also the start-up mode of the multimode wheels), DF Pro, G25, DFGT, G27, G29 (PS3 mode),
       G920 is XInput */
    return vid == 0x046D && (pid == 0xC294 || pid == 0xC298 || pid == 0xC299 || pid == 0xC29A || pid == 0xC29B ||
                             pid == 0xC24F);
}

static void lg_cmd(dev_t *d, uint8_t a, uint8_t b, uint8_t c, uint8_t e, uint8_t f)
{
    const uint8_t r[7] = { a, b, c, e, f, 0, 0 };
    esp_err_t err = send_output(d, 0, r, sizeof(r));
    if (err != ESP_OK) ESP_LOGW(TAG, "wheel command %02X %02X: %s", a, b, esp_err_to_name(err));
}

/* A G25 starts as a Driving Force EX (C294: 10-bit steering, 200 degrees, fewer buttons). Its native-mode command
 * makes it reset its USB connection and come back as C299. true = sent, the device is going away */
static bool lg_native_switch(dev_t *d)
{
    if (d->vid != 0x046D || d->pid != 0xC294) return false;
    const usb_device_desc_t *dd;
    if (!d->out_looked) find_out_ep(d);
    if (!d->out_dev || usb_host_get_device_descriptor(d->out_dev, &dd) != ESP_OK) return false;
    uint16_t bcd = dd->bcdDevice;
    if ((bcd & 0xFF00) != 0x1200) {          /* G25 only (G27 = 0x123x, DFGT 0x13xx need other commands) */
        ESP_LOGI(TAG, "Logitech wheel %04X:%04X bcdDevice %04X: compatibility mode kept", d->vid, d->pid, bcd);
        return false;
    }
    ESP_LOGI(TAG, "Logitech G25 in compatibility mode: switching it to native mode");
    lg_cmd(d, 0xF8, 0x10, 0, 0, 0);
    return true;
}

/* Force feedback on the G25 / G27 (native): the lx_padout task calls it with what changed */
static bool lg_ffb(const dev_t *d)
{
    return d->vid == 0x046D && (d->pid == 0xC299 || d->pid == 0xC29B);
}

/* Centring spring 0..65535 (hid-lg4ff's lg4ff_set_autocenter_default), 0 = off */
static void lg_spring(dev_t *d, uint32_t mag)
{
    if (!mag) {
        lg_cmd(d, 0xF5, 0, 0, 0, 0);
        return;
    }
    uint32_t a, b;
    if (mag <= 0xAAAA) {
        a = 0x0C * mag;
        b = 0x80 * mag;
    } else {
        a = 0x0C * 0xAAAA + 0x06 * (mag - 0xAAAA);
        b = 0x80 * 0xAAAA + 0xFF * (mag - 0xAAAA);
    }
    a >>= 1;
    lg_cmd(d, 0xFE, 0x0D, (uint8_t)(a / 0xAAAA), (uint8_t)(a / 0xAAAA), (uint8_t)(b / 0xAAAA));
    lg_cmd(d, 0x14, 0, 0, 0, 0);
}

static void lg_out(void *ctx, const lx_pad_out_state_t *s)
{
    dev_t *d = ctx;
    if (s->range != d->lg_range) {
        d->lg_range = s->range;
        lg_cmd(d, 0xF8, 0x81, s->range & 0xFF, s->range >> 8, 0);
    }
    if ((int32_t)s->spring != d->lg_spring) {
        d->lg_spring = s->spring;
        lg_spring(d, s->spring);
    }
    int f = s->force;
    if (s->shake) {                         /* rumble: half the wheel's force at most, flipped by the task */
        int amp = (s->strong > s->weak ? s->strong : s->weak) / 2;
        f += s->shake == 1 ? amp : -amp;
    }
    int v = 0x80 + f / 256;                 /* constant force, 0x80 = none (lg4ff_play) */
    v = v < 0 ? 0 : v > 255 ? 255 : v;
    if (v != d->lg_const) {
        d->lg_const = v;
        if (v == 0x80) lg_cmd(d, 0x13, 0, 0, 0, 0);
        else lg_cmd(d, 0x11, 0x08, (uint8_t)v, 0x80, 0);
    }
}

/* Rumble and light bar (output report): DualShock 4 report 0x05, DualSense 0x02 (layouts as in Linux's
 * hid-playstation) */
static void sony_out(void *ctx, const lx_pad_out_state_t *s)
{
    dev_t *d = ctx;
    uint8_t r[64] = { 0 };
    esp_err_t e;
    if (d->pid == 0x0CE6 || d->pid == 0x0DF2) {         /* DualSense (Edge) */
        r[0] = 0x02;
        r[1] = 0x03;                                    /* valid 0: rumble (compatible), haptics select */
        r[2] = 0x04 | 0x10;                             /* valid 1: light bar, player LEDs */
        r[3] = (uint8_t)(s->weak >> 8);
        r[4] = (uint8_t)(s->strong >> 8);
        r[39] = 0x02;                                   /* valid 2: light bar setup (ends the blue start-up glow) */
        r[42] = 0x02;
        r[44] = lx_pad_ps5_leds(s->player);
        r[45] = s->r; r[46] = s->g; r[47] = s->b;
        e = send_output(d, r[0], r, 63);
    } else {
        r[0] = 0x05;
        r[1] = 0x07;                                    /* rumble, light bar, flash */
        r[4] = (uint8_t)(s->weak >> 8);
        r[5] = (uint8_t)(s->strong >> 8);
        r[6] = s->r; r[7] = s->g; r[8] = s->b;
        e = send_output(d, r[0], r, 32);
    }
    if (e != ESP_OK) ESP_LOGW(TAG, "player %d: output report failed: %s", d->player + 1, esp_err_to_name(e));
}

static void parse_pad(dev_t *d, const uint8_t *p, int len)
{
    if (d->fmt == FMT_UNKNOWN) {
        if (len < 4) return;
        d->fmt = detect_format(d, p, len);
        static const char *const names[] = { "?", "PS3", "PS4", "PS5", "generic", "PSX adaptor", "HID descriptor" };
        ESP_LOGI(TAG, "player %d report format: %s (%d bytes)", d->player + 1, names[d->fmt], len);
    }
    if (d->fmt == FMT_HID) {
        parse_hid(d, p, len);
        return;
    }
    if ((d->fmt == FMT_PS4 || d->fmt == FMT_PS5) && d->vid == 0x054C) sony_motion(d, p, len);

    uint32_t b = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint16_t lt = 0, rt = 0;
    switch (d->fmt) {
    case FMT_PS3:
        /* [01][00][btn1][btn2][ps][00][LX][LY][RX][RY] ... pressures from [18] */
        if (len < 10) return;
        lx = AX(p[6]); ly = AX(p[7]); rx = AX(p[8]); ry = AX(p[9]);
        if (p[2] & 0x10) b |= PAPP_BTN_UP;
        if (p[2] & 0x20) b |= PAPP_BTN_RIGHT;
        if (p[2] & 0x40) b |= PAPP_BTN_DOWN;
        if (p[2] & 0x80) b |= PAPP_BTN_LEFT;
        if (p[2] & 0x01) b |= PAPP_BTN_SELECT;
        if (p[2] & 0x02) b |= PAPP_BTN_L3;
        if (p[2] & 0x04) b |= PAPP_BTN_R3;
        if (p[2] & 0x08) b |= PAPP_BTN_START;
        if (p[3] & 0x01) b |= PAPP_BTN_L2;
        if (p[3] & 0x02) b |= PAPP_BTN_R2;
        if (p[3] & 0x04) b |= PAPP_BTN_L1;
        if (p[3] & 0x08) b |= PAPP_BTN_R1;
        if (p[3] & 0x10) b |= PAPP_BTN_Y;       /* Triangle */
        if (p[3] & 0x20) b |= PAPP_BTN_B;       /* Circle   */
        if (p[3] & 0x40) b |= PAPP_BTN_A;       /* Cross    */
        if (p[3] & 0x80) b |= PAPP_BTN_X;       /* Square   */
        if (p[4] & 0x01) b |= PAPP_BTN_HOME;
        if (len >= 20) { lt = TRIG(p[18]); rt = TRIG(p[19]); }
        break;
    case FMT_PS4:
        /* [01][LX][LY][RX][RY][hat+btn1][btn2][btn3][L2][R2] */
        if (len < 10) return;
        lx = AX(p[1]); ly = AX(p[2]); rx = AX(p[3]); ry = AX(p[4]);
        b = hat_to_dpad(p[5] & 0x0F) | ps_buttons(p[5], p[6], p[7]);
        lt = TRIG(p[8]); rt = TRIG(p[9]);
        break;
    case FMT_PS5:
        /* [01][LX][LY][RX][RY][L2][R2][cnt][hat+btn1][btn2][btn3] */
        if (len < 11) return;
        lx = AX(p[1]); ly = AX(p[2]); rx = AX(p[3]); ry = AX(p[4]);
        lt = TRIG(p[5]); rt = TRIG(p[6]);
        b = hat_to_dpad(p[8] & 0x0F) | ps_buttons(p[8], p[9], p[10]);
        break;
    case FMT_PSX_ADAPTOR:
        /* [01][80][80][HatX][HatY][XABY][Sel,Start|LX,RX][00] */
        lx = AX(p[1]); ly = AX(p[2]); rx = AX(p[3]); ry = AX(p[4]);
        b = axes_to_dpad(p[3], p[4], 64, 192) | ps_buttons(p[5], p[6], p[7]);
        break;
    default: {
        /* [LX][LY][RX][RY][Z][hat|XABY][L1 R1 . . Sel Start][..], optional report id */
        int o = (len > 8 && p[0] >= 0x01 && p[0] <= 0x0F) ? 1 : 0;
        if (len - o < 4) return;
        lx = AX(p[o]); ly = AX(p[o + 1]); rx = AX(p[o + 2]); ry = AX(p[o + 3]);
        b = axes_to_dpad(p[o], p[o + 1], 0x20, 0xE0);
        if (len - o >= 6) {
            uint8_t h = p[o + 5];
            if ((h & 0x0F) <= 7) b |= hat_to_dpad(h & 0x0F);
            if (h & 0x10) b |= PAPP_BTN_X;
            if (h & 0x20) b |= PAPP_BTN_A;
            if (h & 0x40) b |= PAPP_BTN_B;
            if (h & 0x80) b |= PAPP_BTN_Y;
        }
        if (len - o >= 7) {
            uint8_t s = p[o + 6];
            if (s & 0x01) b |= PAPP_BTN_L1;
            if (s & 0x02) b |= PAPP_BTN_R1;
            if (s & 0x10) b |= PAPP_BTN_SELECT;
            if (s & 0x20) b |= PAPP_BTN_START;
        }
        break;
    }
    }
    /* A genuine DS3 stays silent after connecting until it is pressed once,
     * in practice with PS (= HOME). That press wakes the pad; it must not
     * also stop the running app. HOME counts again once PS was released. */
    if (d->waking) {
        if (b & PAPP_BTN_HOME) b &= ~PAPP_BTN_HOME;
        else {
            d->waking = false;
            ESP_LOGI(TAG, "player %d: DS3 is awake", d->player + 1);
        }
    }
    lx_input_pad_update(d->player, b, lx, ly, rx, ry, lt, rt);
}

/* ── HID callbacks ──────────────────────────────────────────────────────── */

static void close_dev(dev_t *d)
{
    if (d->kind == DEV_PAD) lx_input_pad_detach(d->player);    /* (waits for an output send in progress) */
    if (d->kind == DEV_KBD) lx_input_kbd_detach();
    lx_input_usb_dev_close(d->out_dev);
    hid_host_device_close(d->handle);
    memset(d, 0, sizeof(*d));
}

static void interface_cb(hid_host_device_handle_t h, const hid_host_interface_event_t ev, void *arg)
{
    dev_t *d = find_dev(h);
    switch (ev) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t data[64];
        size_t len = 0;
        if (!d || hid_host_device_get_raw_input_report_data(h, data, sizeof(data), &len) != ESP_OK || !len)
            break;
        if (d->kind == DEV_PAD) parse_pad(d, data, (int)len);
        else if (d->kind == DEV_KBD && len >= 8) lx_input_kbd_report(data[0], data + 2);
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        if (d) close_dev(d);
        else hid_host_device_close(h);
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        /* The device going away (DISCONNECTED follows from the USB core), or a
           failed transfer: hid_host doesn't resubmit, so lx_hid restarts it */
        ESP_LOGW(TAG, "transfer error");
        if (d) xQueueSend(s_restart_q, &h, 0);
        break;
    default:
        break;
    }
}

static void driver_cb(hid_host_device_handle_t h, const hid_host_driver_event_t ev, void *arg)
{
    if (ev == HID_HOST_DRIVER_EVENT_CONNECTED) xQueueSend(s_connect_q, &h, 0);
}

/* ── Device bring-up ────────────────────────────────────────────────────── */

/* Top-level collection Generic Desktop / Joystick or Gamepad? */
static bool desc_is_gamepad(const uint8_t *desc, size_t len)
{
    /* Any collection: a receiver may list a mouse or keyboard before the pad */
    for (size_t i = 0; i + 3 < len; i++) {
        if (desc[i] == 0x05 && desc[i + 1] == 0x01 && desc[i + 2] == 0x09 &&
            (desc[i + 3] == 0x04 || desc[i + 3] == 0x05))
            return true;
    }
    return false;
}

/* Genuine DualShock 3 only streams after a feature request; it may still need
 * one physical button press (see the reference project's notes). */
static void ds3_enable(hid_host_device_handle_t h)
{
    static uint8_t led[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
        0xff, 0x27, 0x10, 0x00, 0x32, 0xff, 0x27, 0x10, 0x00, 0x32,
        0xff, 0x27, 0x10, 0x00, 0x32, 0xff, 0x27, 0x10, 0x00, 0x32,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    uint8_t f2[17];
    size_t f2_len = sizeof(f2);
    hid_class_request_get_report(h, HID_REPORT_TYPE_FEATURE, 0xF2, f2, &f2_len);
    uint8_t enable[4] = { 0x42, 0x0C, 0x00, 0x00 };
    esp_err_t err = hid_class_request_set_report(h, HID_REPORT_TYPE_FEATURE, 0xF4, enable, sizeof(enable));
    led[9] = 0x02;
    hid_class_request_set_report(h, HID_REPORT_TYPE_OUTPUT, 0x01, led, sizeof(led));
    ESP_LOGI(TAG, "DS3 operational mode %s — it stays silent until pressed once (PS is fine)",
             err == ESP_OK ? "enabled" : "request failed");
}

static void product_name(hid_host_device_handle_t h, uint16_t vid, uint16_t pid, char *out, size_t len)
{
    if (vid == 0x054C && pid == 0x0268) { snprintf(out, len, "DualShock 3"); return; }
    if (vid == 0x054C && (pid == 0x05C4 || pid == 0x09CC)) { snprintf(out, len, "DualShock 4"); return; }
    if (vid == 0x054C && pid == 0x0CE6) { snprintf(out, len, "DualSense"); return; }
    hid_host_dev_info_t info;
    size_t n = 0;
    if (hid_host_get_device_info(h, &info) == ESP_OK) {
        for (int i = 0; i < HID_STR_DESC_MAX_LENGTH && info.iProduct[i] && n + 1 < len; i++)
            out[n++] = info.iProduct[i] < 0x80 ? (char)info.iProduct[i] : '?';
    }
    out[n] = 0;
    if (!n) snprintf(out, len, "Gamepad %04X:%04X", vid, pid);
}

static void open_device(hid_host_device_handle_t h)
{
    dev_t *d = NULL;
    for (int i = 0; i < MAX_DEVS; i++) if (s_devs[i].kind == DEV_FREE) { d = &s_devs[i]; break; }
    hid_host_dev_params_t params;
    if (!d || hid_host_device_get_params(h, &params) != ESP_OK) return;

    const hid_host_device_config_t cfg = { .callback = interface_cb };
    if (hid_host_device_open(h, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "open failed");
        return;
    }
    hid_host_dev_info_t info = { 0 };
    hid_host_get_device_info(h, &info);
    *d = (dev_t){ .handle = h, .kind = DEV_IGNORED, .player = -1, .vid = info.VID, .pid = info.PID };

    if (params.proto == HID_PROTOCOL_KEYBOARD) {
        d->kind = DEV_KBD;
        if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE)
            hid_class_request_set_protocol(h, HID_REPORT_PROTOCOL_BOOT);
        hid_class_request_set_idle(h, 0, 0);
    } else if (params.proto == HID_PROTOCOL_NONE) {
        size_t dlen = 0;
        const uint8_t *desc = hid_host_get_report_descriptor(h, &dlen);
        d->has_layout = desc && hid_layout_parse(desc, dlen, &d->layout);
        /* No descriptor: assume a gamepad, like the reference did. A joystick, wheel or pedals: the parser
           found one */
        if (!desc || d->has_layout || desc_is_gamepad(desc, dlen)) d->kind = DEV_PAD;
        else ESP_LOG_BUFFER_HEX_LEVEL(TAG, desc, dlen < 128 ? dlen : 128, ESP_LOG_DEBUG);   /* to see what it is */
        if (desc && d->kind == DEV_PAD) {
            static const char *const kinds[] = { "gamepad", "flight stick", "throttle", "wheel" };
            d->pkind = d->has_layout ? lx_pad_kind(&d->layout) : PAPP_PAD_GAMEPAD;
            hid_state_init(&d->raw);
            int axes = 0;
            for (int a = 0; a < HID_AX_COUNT; a++) axes += hid_has(&d->layout, a);
            ESP_LOGI(TAG, "report descriptor %u bytes: %s, %s, %d axes, %d buttons, %d hats, ids %d", (unsigned)dlen,
                     d->has_layout ? "usable" : "not usable", kinds[d->pkind & 3], axes, d->layout.buttons,
                     d->layout.hats, d->layout.nids > 1 ? -d->layout.nids : d->layout.report_id);
        }
    }
    ESP_LOGI(TAG, "%04X:%04X iface proto %d -> %s", d->vid, d->pid, params.proto,
             d->kind == DEV_PAD ? "gamepad" : d->kind == DEV_KBD ? "keyboard" : "ignored");

    /* PS4 pads can droop VBUS on configuration; let the supply settle (reference) */
    if (d->kind == DEV_PAD) vTaskDelay(pdMS_TO_TICKS(500));

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3 && err != ESP_OK; attempt++) {
        err = hid_host_device_start(h);
        if (err != ESP_OK) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        hid_host_device_close(h);
        memset(d, 0, sizeof(*d));
        return;
    }

    if (d->kind == DEV_PAD && lg_native_switch(d)) {
        d->kind = DEV_IGNORED;              /* it resets and comes back with its native id */
        return;
    }
    if (d->kind == DEV_PAD) {
        char name[24];
        product_name(h, d->vid, d->pid, name, sizeof(name));
        /* Logitech wheels name their pedals X/Y/Z/Rz, not Accelerator / Brake */
        if (d->has_layout && logitech_wheel(d->vid, d->pid)) d->pkind = PAPP_PAD_WHEEL;
        /* (pkind is a gamepad for the pads read by model: the layout isn't used for them) */
        int kind = d->has_layout && d->vid != 0x054C ? d->pkind : PAPP_PAD_GAMEPAD;
        d->pkind = kind;
        d->player = lx_input_pad_attach_kind(name, d->vid, d->pid, kind);
        if (d->player >= 0 && d->has_layout) {
            int k = lx_input_pad_ext(d->player, &d->ext);   /* kind= of gamepads.txt applied */
            if (k >= 0) d->pkind = k;
            d->ext_gen = lx_input_pad_ext_gen(d->player);
        }
        if (d->player < 0) d->kind = DEV_IGNORED;           /* all player slots taken */
        else if (d->vid == 0x054C && d->pid == 0x0268) {
            d->waking = true;
            ds3_enable(h);
        } else if (d->vid == 0x054C && (d->pid == 0x05C4 || d->pid == 0x09CC || d->pid == 0x0CE6 || d->pid == 0x0DF2)) {
            lx_input_pad_set_out(d->player, sony_out, d);
        } else if (lg_ffb(d)) {
            d->lg_range = d->lg_spring = d->lg_const = -1;      /* nothing sent yet */
            lx_input_pad_set_out(d->player, lg_out, d);
            lx_input_pad_set_ffb(d->player, true);
        }
    } else if (d->kind == DEV_KBD) {
        lx_input_kbd_attach();
    }
}

/* After a failed transfer: give a disconnect time to arrive, then restart the
 * interface (halt, flush, clear the endpoint, resubmit) if the device is still here */
static void restart_device(hid_host_device_handle_t h)
{
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!find_dev(h)) return;
    esp_err_t e = hid_host_device_stop(h);
    if (e == ESP_OK) e = hid_host_device_start(h);
    ESP_LOGW(TAG, "restarted device after a transfer error: %s", esp_err_to_name(e));
}

static void hid_task(void *arg)
{
    uint32_t bna_seen = 0;
    for (;;) {
        hid_host_device_handle_t h;
        if (xQueueReceive(s_connect_q, &h, pdMS_TO_TICKS(200))) open_device(h);
        while (xQueueReceive(s_restart_q, &h, 0)) restart_device(h);
        if (s_bna_count != bna_seen) {
            bna_seen = s_bna_count;
            ESP_LOGW(TAG, "USB channel error without halt (BNA workaround), %lu so far", (unsigned long)bna_seen);
        }
    }
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t cfg = { .intr_flags = ESP_INTR_FLAG_LEVEL3 };
    esp_err_t err = usb_host_install(&cfg);
    xTaskNotify((TaskHandle_t)arg, err, eSetValueWithOverwrite);
    if (err != ESP_OK) vTaskDelete(NULL);
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            /* A failed root-port enumeration can leave the port disabled until
             * another physical disconnect. Power-cycle it after the device tree
             * has been fully released so a hub reconnect can enumerate again. */
            esp_err_t power_err = usb_host_lib_set_root_port_power(false);
            if (power_err == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(250));
                power_err = usb_host_lib_set_root_port_power(true);
            }
            if (power_err != ESP_OK) {
                ESP_LOGW(TAG, "USB root-port recovery failed: %s", esp_err_to_name(power_err));
            } else {
                ESP_LOGI(TAG, "USB root port power-cycled after all devices were freed");
            }
        }
    }
}

esp_err_t lx_input_usb_start(void)
{
    s_connect_q = xQueueCreate(8, sizeof(hid_host_device_handle_t));
    s_restart_q = xQueueCreate(8, sizeof(hid_host_device_handle_t));
    /* USB host events (also a Bluetooth dongle's): above lwIP (18), a Wi-Fi download must not hold them up */
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_host", 4096, xTaskGetCurrentTaskHandle(), 19, NULL, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    uint32_t err = ESP_ERR_TIMEOUT;
    xTaskNotifyWait(0, 0, &err, pdMS_TO_TICKS(3000));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB host install failed: %s", esp_err_to_name(err));
        return err;
    }
    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = 6,
        .stack_size = 4096,
        .core_id = 0,
        .callback = driver_cb,
    };
    esp_err_t e = hid_host_install(&hid_cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "HID host install failed: %s", esp_err_to_name(e));
        return e;
    }
    xTaskCreatePinnedToCore(hid_task, "lx_hid", 4096, NULL, 6, NULL, 1);
    if (lx_input_xinput_start() != ESP_OK) ESP_LOGW(TAG, "XInput client failed");
    ESP_LOGI(TAG, "USB HID host ready");
    return ESP_OK;
}
