#include "lx_hid_parse.h"

#include <limits.h>
#include <string.h>

#define PAGE_DESKTOP 0x01
#define PAGE_SIM     0x02
#define PAGE_BUTTON  0x09
#define USE_X        0x30
#define USE_SLIDER   0x36
#define USE_DIAL     0x37
#define USE_WHEEL    0x38
#define USE_HAT      0x39
#define MAX_LOCAL    24
#define MAX_RIDS     8           /* report ids followed while counting bits */
#define MAX_STACK    4

typedef struct {
    uint16_t page;
    int32_t  log_min, log_max;
    uint32_t size, count;
    uint8_t  id;
} globals_t;

static uint32_t item_u(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static int32_t item_s(const uint8_t *p, int n)
{
    uint32_t v = item_u(p, n);
    if (n == 1) return (int8_t)v;
    if (n == 2) return (int16_t)v;
    return (int32_t)v;
}

/* Bits used so far per report id */
static uint32_t *offset_of(uint8_t *ids, uint32_t *bits, int *n, uint8_t id)
{
    for (int i = 0; i < *n; i++)
        if (ids[i] == id) return &bits[i];
    if (*n == MAX_RIDS) return NULL;
    ids[*n] = id;
    bits[*n] = 0;
    return &bits[(*n)++];
}

/* Role of a (page, usage) axis, -1 if it isn't one */
static int axis_role(uint32_t page, uint32_t use, const hid_layout_t *out)
{
    if (page == PAGE_DESKTOP) {
        if (use >= USE_X && use < USE_X + 6) return HID_AX_X + (int)(use - USE_X);
        if (use == USE_SLIDER) return out->axis[HID_AX_SLIDER].size ? HID_AX_SLIDER2 : HID_AX_SLIDER;
        if (use == USE_DIAL) return HID_AX_DIAL;
        if (use == USE_WHEEL) return HID_AX_WHEEL;
    } else if (page == PAGE_SIM) {
        switch (use) {
        case 0xBB: return HID_AX_THROTTLE;
        case 0xBA: return HID_AX_RUDDER;
        case 0xC8: return HID_AX_STEERING;
        case 0xC4: return HID_AX_ACCEL;
        case 0xC5: return HID_AX_BRAKE;
        }
    }
    return -1;
}

static bool note_id(hid_layout_t *out, uint8_t id)
{
    for (int i = 0; i < out->nids; i++)
        if (out->ids[i] == id) return true;
    if (out->nids == HID_MAX_IDS) return false;
    if (!out->nids) out->report_id = id;
    out->ids[out->nids++] = id;
    return true;
}

static void add_field(hid_layout_t *out, const globals_t *g, uint32_t page, uint32_t use, uint32_t at)
{
    hid_field_t f = { (uint16_t)at, (uint8_t)g->size, g->id, g->log_min < 0, g->log_min, g->log_max };
    /* A 1-byte logical max of 0xFF reads as -1: many pads mean 255 */
    if (g->log_min >= 0 && g->log_max < 0)
        f.max = g->size >= 31 ? INT32_MAX : (int32_t)((1u << g->size) - 1);

    int role;
    if (page == PAGE_BUTTON && use >= 1 && use <= HID_MAX_BUTTONS && g->size == 1) {
        if (!note_id(out, g->id)) return;
        out->button[use - 1] = f;
        if (use > out->buttons) out->buttons = (uint8_t)use;
    } else if (page == PAGE_DESKTOP && use == USE_HAT) {
        if (out->hats == HID_MAX_HATS || !note_id(out, g->id)) return;
        out->hat[out->hats++] = f;
    } else if ((role = axis_role(page, use, out)) >= 0) {
        if (!note_id(out, g->id)) return;
        out->axis[role] = f;    /* a repeated usage: the last one wins */
    }
}

/* An application collection we read: joystick, gamepad, multi-axis controller, or a simulation device */
static int pad_collection(uint32_t page, uint32_t use)
{
    if (page == PAGE_DESKTOP && use == 0x04) return HID_DEV_JOYSTICK;
    if (page == PAGE_DESKTOP && use == 0x05) return HID_DEV_GAMEPAD;
    if (page == PAGE_DESKTOP && use == 0x08) return HID_DEV_MULTIAXIS;
    if (page == PAGE_SIM && use >= 0x01 && use <= 0x0C) return HID_DEV_SIMULATION;
    return -1;
}

static bool any_axis(const hid_layout_t *l)
{
    for (int i = 0; i < HID_AX_COUNT; i++)
        if (l->axis[i].size) return true;
    return false;
}

bool hid_layout_parse(const uint8_t *d, size_t len, hid_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    globals_t g = { 0 }, stack[MAX_STACK];
    int sp = 0;
    /* usages as page << 16 | id; page 0 = the usage page in force at the main item */
    uint32_t usages[MAX_LOCAL], use_min = 0, use_max = 0;
    int nuse = 0;
    uint8_t ids[MAX_RIDS];
    uint32_t bits[MAX_RIDS];
    int nids = 0;
    bool in_pad = false, done = false;
    int depth = 0, pad_depth = 0;

    for (size_t i = 0; i < len;) {
        uint8_t b = d[i++];
        if (b == 0xFE) {                        /* long item */
            if (i >= len) break;
            i += 2u + d[i];
            continue;
        }
        int n = (b & 3) == 3 ? 4 : (b & 3);
        if (i + n > len) break;
        const uint8_t *p = d + i;
        i += n;
        int type = (b >> 2) & 3, tag = b >> 4;

        if (type == 1) {                        /* global */
            switch (tag) {
            case 0: g.page = (uint16_t)item_u(p, n); break;
            case 1: g.log_min = item_s(p, n); break;
            case 2: g.log_max = item_s(p, n); break;
            case 7: g.size = item_u(p, n); break;
            case 8: g.id = (uint8_t)item_u(p, n); break;
            case 9: g.count = item_u(p, n); break;
            case 10: if (sp < MAX_STACK) stack[sp++] = g; break;
            case 11: if (sp > 0) g = stack[--sp]; break;
            }
        } else if (type == 2) {                 /* local */
            uint32_t v = item_u(p, n);
            if (n < 4) v &= 0xFFFF;             /* 4 bytes: extended usage, page in the top half */
            if (tag == 0 && nuse < MAX_LOCAL) usages[nuse++] = v;
            else if (tag == 1) use_min = v;
            else if (tag == 2) use_max = v;
        } else if (type == 0) {                 /* main */
            if (tag == 10) {                    /* collection */
                depth++;
                bool app = n && p[0] == 1;
                uint32_t u = nuse ? usages[nuse - 1] : 0;
                uint32_t page = u >> 16 ? u >> 16 : g.page;
                int dev = pad_collection(page, u & 0xFFFF);
                if (!in_pad && !done && app && dev >= 0) {
                    in_pad = true;
                    pad_depth = depth;
                    out->device = (uint8_t)dev;
                }
            } else if (tag == 12) {             /* end collection */
                if (in_pad && depth == pad_depth) {
                    in_pad = false;
                    /* the first pad collection with controls is the one; a second player's is ignored */
                    done = out->nids != 0;
                }
                depth--;
            } else if (tag == 8) {              /* input */
                uint32_t *off = offset_of(ids, bits, &nids, g.id);
                if (!off) return false;
                uint32_t flags = n ? item_u(p, n) : 0;
                bool data_var = !(flags & 1) && (flags & 2);
                if (in_pad && data_var) {
                    for (uint32_t f = 0; f < g.count; f++) {
                        uint32_t at = *off + f * g.size;
                        uint32_t u = nuse ? usages[f < (uint32_t)nuse ? f : (uint32_t)nuse - 1]
                                          : use_min + f;
                        if (!nuse && (use_min & 0xFFFF) + f > (use_max & 0xFFFF)) continue;
                        if (at + g.size > 0xFFFF) return false;
                        uint32_t page = u >> 16 ? u >> 16 : g.page;
                        add_field(out, &g, page, u & 0xFFFF, at);
                    }
                }
                *off += g.count * g.size;
            }
            nuse = 0;
            use_min = use_max = 0;
        }
    }
    bool stick = out->axis[HID_AX_X].size && out->axis[HID_AX_Y].size;
    if (out->buttons >= 2 && (stick || out->hats)) return true;
    /* Joysticks, throttles, wheels and pedals may have few buttons, or none */
    return out->device != HID_DEV_GAMEPAD && any_axis(out);
}

static uint32_t get_bits(const uint8_t *p, size_t len, uint32_t bit, uint32_t size)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < size && i < 32; i++) {
        uint32_t at = bit + i;
        if (at / 8 < len && (p[at / 8] >> (at % 8) & 1)) v |= 1u << i;
    }
    return v;
}

static int32_t field_value(const hid_field_t *f, const uint8_t *p, size_t len)
{
    uint32_t v = get_bits(p, len, f->bit, f->size);
    if (f->sign && f->size < 32 && (v >> (f->size - 1) & 1)) v |= ~0u << f->size;
    return (int32_t)v;
}

void hid_state_init(hid_state_t *s)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < HID_MAX_HATS; i++) s->hat[i] = -1;
}

bool hid_layout_decode(const hid_layout_t *l, const uint8_t *r, size_t len, hid_state_t *out)
{
    uint8_t id = 0;
    if (l->report_id) {
        if (len < 2) return false;
        id = r[0];
        r++;
        len--;
    }
    bool ours = false;
    for (int i = 0; i < l->nids; i++) ours |= l->ids[i] == id;
    if (!ours) return false;

    for (int i = 0; i < HID_AX_COUNT; i++) {
        const hid_field_t *f = &l->axis[i];
        if (!f->size || f->id != id) continue;
        if (f->max <= f->min) continue;
        int64_t v = field_value(f, r, len);
        if (v < f->min) v = f->min;
        if (v > f->max) v = f->max;
        out->axis[i] = (int16_t)((v - f->min) * 65535 / ((int64_t)f->max - f->min) - 32768);
    }
    for (int h = 0; h < l->hats; h++) {
        const hid_field_t *f = &l->hat[h];
        if (f->id != id) continue;
        int32_t v = field_value(f, r, len) - f->min;
        int32_t range = f->max - f->min + 1;
        out->hat[h] = -1;
        if (v >= 0 && v < range && range == 8) out->hat[h] = (int8_t)v;
        else if (v >= 0 && v < range && range == 4) out->hat[h] = (int8_t)(v * 2);
    }
    for (int i = 0; i < l->buttons; i++) {
        const hid_field_t *f = &l->button[i];
        if (!f->size || f->id != id) continue;
        if (get_bits(r, len, f->bit, 1)) out->buttons |= 1u << i;
        else out->buttons &= ~(1u << i);
    }
    return true;
}
