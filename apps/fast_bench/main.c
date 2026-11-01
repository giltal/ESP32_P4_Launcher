/*
 * fast_bench — PSRAM vs FAST (internal SRAM) code and data placement (Spike B).
 *   args: [rounds] (default 3; the best time of each kernel is reported)
 * The same kernels (kernels.inc) are built twice: _s runs from PSRAM, _f has
 * code and tables tagged PAPP_FAST / PAPP_FAST_DATA. Results must match.
 * Exit code = number of mismatches.
 */
#include "papp/papp.h"

PAPP_MANIFEST(.id = "test.fastbench", .name = "FAST bench", .version = "0.1.0", .main_stack = 8192, .flags = PAPP_MF_HIDDEN);

#define SUF  _s
#define CODE
#define DATA
#include "kernels.inc"
#undef SUF
#undef CODE
#undef DATA

#define SUF  _f
#define CODE PAPP_FAST
#define DATA PAPP_FAST_DATA
#include "kernels.inc"

#define BUF_BYTES (64 * 1024)
#define BLIT_PX   (320 * 240)
#define STREAM_BYTES (2 * 1024 * 1024)

typedef struct { const char *name; int64_t s, f; uint32_t rs, rf; } result_t;

static int64_t best(int64_t a, int64_t b) { return !a || b < a ? b : a; }

static int64_t now(void) { return papp_time_us(); }

int main(int argc, char **argv)
{
    int rounds = 3;
    if (argc > 1) {
        rounds = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) rounds = rounds * 10 + (*p - '0');
        if (rounds < 1) rounds = 1;
    }

    uint8_t *buf = papp_alloc(BUF_BYTES, PAPP_MEM_LARGE, 64);
    uint8_t *src = papp_alloc(BLIT_PX, PAPP_MEM_LARGE, 64);
    uint16_t *dst = papp_alloc(BLIT_PX * 2, PAPP_MEM_LARGE, 64);
    uint8_t *stream = papp_alloc(STREAM_BYTES, PAPP_MEM_LARGE, 64);
    if (!buf || !src || !dst || !stream) {
        papp_log("out of memory\n");
        return 99;
    }
    for (int i = 0; i < BUF_BYTES; i++) buf[i] = (uint8_t)(i * 7 + (i >> 9));
    for (int i = 0; i < BLIT_PX; i++) src[i] = (uint8_t)(i * 13);
    for (int i = 0; i < STREAM_BYTES; i++) stream[i] = (uint8_t)(i ^ (i >> 11));
    for (int i = 0; i < 256; i++) pal_s[i] = pal_f[i] = (uint16_t)(i * 0x0101 ^ 0x5A5A);
    crc_init_s();
    crc_init_f();

    papp_log("vm_run_f at %p, vm_run_s at %p, crc_tab_f at %p\n", (void *)vm_run_f, (void *)vm_run_s,
             (void *)crc_tab_f);

    result_t r[7] = { { "interpreter 200k ops" }, { "crc32 64 KB" }, { "blit 320x240" }, { "32 KB code x200" },
                      { "100 frames + 32KB I/O" }, { "100 frames +128KB I/O" }, { "100 frames +512KB I/O" } };
    static const size_t chunk[3] = { 32 * 1024, 128 * 1024, 512 * 1024 };
    for (int k = 0; k < rounds; k++) {
        int64_t t;
        t = now(); r[0].rs = vm_run_s(200000 / 14); r[0].s = best(r[0].s, now() - t);
        t = now(); r[0].rf = vm_run_f(200000 / 14); r[0].f = best(r[0].f, now() - t);
        t = now(); r[1].rs = crc_run_s(buf, BUF_BYTES); r[1].s = best(r[1].s, now() - t);
        t = now(); r[1].rf = crc_run_f(buf, BUF_BYTES); r[1].f = best(r[1].f, now() - t);
        t = now(); blit_s(dst, src, BLIT_PX); r[2].s = best(r[2].s, now() - t);
        r[2].rs = dst[1234] ^ dst[BLIT_PX - 1];
        t = now(); blit_f(dst, src, BLIT_PX); r[2].f = best(r[2].f, now() - t);
        r[2].rf = dst[1234] ^ dst[BLIT_PX - 1];
        t = now(); r[3].rs = big_run_s(200); r[3].s = best(r[3].s, now() - t);
        t = now(); r[3].rf = big_run_f(200); r[3].f = best(r[3].f, now() - t);
        for (int c = 0; c < 3; c++) {
            result_t *q = &r[4 + c];
            t = now(); q->rs = frames_s(100, stream, STREAM_BYTES, chunk[c]); q->s = best(q->s, now() - t);
            t = now(); q->rf = frames_f(100, stream, STREAM_BYTES, chunk[c]); q->f = best(q->f, now() - t);
        }
    }

    int bad = 0;
    for (int i = 0; i < 7; i++) {
        bool same = r[i].rs == r[i].rf;
        bad += !same;
        papp_log("%-22s PSRAM %7lld us  FAST %7lld us  x%lu.%02lu%s\n", r[i].name, r[i].s, r[i].f,
                 (unsigned long)(r[i].s / r[i].f), (unsigned long)(r[i].s * 100 / r[i].f % 100),
                 same ? "" : "  RESULT MISMATCH");
    }
    return bad;
}
