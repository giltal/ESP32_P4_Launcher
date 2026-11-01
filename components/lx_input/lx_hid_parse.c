#include "lx_hid_parse.h"

#include <limits.h>
#include <string.h>

#define PAGE_DESKTOP 0x01
#define PAGE_BUTTON  0x09
#define USE_X        0x30
#define USE_HAT      0x39
#define MAX_LOCAL    24
#define MAX_IDS      8
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
    if (*n == MAX_IDS) return NULL;
    ids[*n] = id;
    bits[*n] = 0;
    return &bits[(*n)++];
}

static void add_field(hid_layout_t *out, const globals_t *g, uint32_t use, uint32_t at)
{
    hid_field_t f = { (uint16_t)at, (uint8_t)g->size, g->log_min < 0, g->log_min, g->log_max };
    /* A 1-byte logical max of 0xFF reads as -1: many pads mean 255 */
    if (g->log_min >= 0 && g->log_max < 0)
        f.max = g->size >= 31 ? INT32_MAX : (int32_t)((1u << g->size) - 1);

    if (g->page == PAGE_BUTTON && use >= 1 && use <= HID_MAX_BUTTONS && g->size == 1) {
        if (!out->buttons) out->button_bit = (uint16_t)(at - (use - 1));
        if (use > out->buttons) out->buttons = (uint8_t)use;
    } else if (g->page == PAGE_DESKTOP && use >= USE_X && use < USE_X + HID_AX_COUNT) {
        out->axis[use - USE_X] = f;
    } else if (g->page == PAGE_DESKTOP && use == USE_HAT) {
        out->hat = f;
    } else {
        return;
    }
    out->report_id = g->id;
}

bool hid_layout_parse(const uint8_t *d, size_t len, hid_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    globals_t g = { 0 }, stack[MAX_STACK];
    int sp = 0;
    uint32_t usages[MAX_LOCAL], use_min = 0, use_max = 0;
    int nuse = 0;
    uint8_t ids[MAX_IDS];
    uint32_t bits[MAX_IDS];
    int nids = 0;
    bool in_pad = false, found = false;
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
            uint32_t v = item_u(p, n) & 0xFFFF;
            if (tag == 0 && nuse < MAX_LOCAL) usages[nuse++] = v;
            else if (tag == 1) use_min = v;
            else if (tag == 2) use_max = v;
        } else if (type == 0) {                 /* main */
            if (tag == 10) {                    /* collection */
                depth++;
                bool app = n && p[0] == 1;
                uint32_t use = nuse ? usages[nuse - 1] : 0;
                if (!in_pad && !found && app && g.page == PAGE_DESKTOP &&
                    (use == 0x04 || use == 0x05)) {
                    in_pad = true;
                    pad_depth = depth;
                }
            } else if (tag == 12) {             /* end collection */
                if (in_pad && depth == pad_depth) in_pad = false;
                depth--;
            } else if (tag == 8) {              /* input */
                uint32_t *off = offset_of(ids, bits, &nids, g.id);
                if (!off) return false;
                uint32_t flags = n ? item_u(p, n) : 0;
                bool data_var = !(flags & 1) && (flags & 2);
                bool same_report = !found || g.id == out->report_id;
                if (in_pad && data_var && same_report) {
                    for (uint32_t f = 0; f < g.count; f++) {
                        uint32_t at = *off + f * g.size;
                        uint32_t use = nuse ? usages[f < (uint32_t)nuse ? f : (uint32_t)nuse - 1]
                                            : use_min + f;
                        if (!nuse && use > use_max) continue;
                        if (at + g.size > 0xFFFF) return false;
                        add_field(out, &g, use, at);
                    }
                    found = out->buttons || out->hat.size || out->axis[HID_AX_X].size;
                }
                *off += g.count * g.size;
            }
            nuse = 0;
            use_min = use_max = 0;
        }
    }
    bool stick = out->axis[HID_AX_X].size && out->axis[HID_AX_Y].size;
    return out->buttons >= 2 && (stick || out->hat.size);
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

bool hid_layout_decode(const hid_layout_t *l, const uint8_t *r, size_t len, hid_state_t *out)
{
    memset(out, 0, sizeof(*out));
    out->hat = -1;
    if (l->report_id) {
        if (len < 2 || r[0] != l->report_id) return false;
        r++;
        len--;
    }
    for (int i = 0; i < HID_AX_COUNT; i++) {
        const hid_field_t *f = &l->axis[i];
        if (!f->size || f->max <= f->min) continue;
        int64_t v = field_value(f, r, len);
        if (v < f->min) v = f->min;
        if (v > f->max) v = f->max;
        out->axis[i] = (int16_t)((v - f->min) * 65535 / ((int64_t)f->max - f->min) - 32768);
    }
    if (l->hat.size) {
        int32_t v = field_value(&l->hat, r, len) - l->hat.min;
        int32_t range = l->hat.max - l->hat.min + 1;
        if (v >= 0 && v < range && range == 8) out->hat = (int8_t)v;
        else if (v >= 0 && v < range && range == 4) out->hat = (int8_t)(v * 2);
    }
    for (int i = 0; i < l->buttons; i++)
        if (get_bits(r, len, l->button_bit + (uint32_t)i, 1)) out->buttons |= (uint16_t)(1u << i);
    return true;
}
