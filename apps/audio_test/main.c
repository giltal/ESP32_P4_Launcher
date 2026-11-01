/*
 * audio_test — AUDI interface test.
 *   mix [secs]      four streams at once (11025 mono U8, 22050 mono S16,
 *                   32000 stereo S16, 44100 stereo S16) playing a C major
 *                   chord; reports each stream's measured playback rate
 *   cycles [n]      open / write / close n times, checks every open works
 *   block           pause a stream and block in write() until STOP
 * Exit code = number of failed checks.
 */
#include "papp/papp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

PAPP_MANIFEST(.id = "test.audio", .name = "Audio test", .version = "0.1.0", .main_stack = 16384, .flags = PAPP_MF_HIDDEN);

#define NSTREAMS 4
#define CHUNK_MS 10

typedef struct {
    papp_audio_cfg_t cfg;
    float freq, phase;
    papp_audio_stream_t *s;
    uint8_t buf[44100 * CHUNK_MS / 1000 * 4 + 16];
    int pending;                    /* frames in buf not yet accepted */
    int off;
} gen_t;

static int s_fail;

#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; papp_log("FAIL: " __VA_ARGS__); papp_log("\n"); } } while (0)

static void generate(gen_t *g)
{
    int n = g->cfg.rate * CHUNK_MS / 1000;
    float step = 2.0f * (float)M_PI * g->freq / g->cfg.rate;
    for (int i = 0; i < n; i++) {
        float v = sinf(g->phase) * 0.18f;
        g->phase += step;
        if (g->phase > 2.0f * (float)M_PI) g->phase -= 2.0f * (float)M_PI;
        if (g->cfg.format == PAPP_AUDIO_U8) {
            g->buf[i] = (uint8_t)(128 + (int)(v * 127));
        } else {
            int16_t *p = (int16_t *)g->buf + i * g->cfg.channels;
            p[0] = (int16_t)(v * 32767);
            if (g->cfg.channels == 2) p[1] = p[0];
        }
    }
    g->pending = n;
    g->off = 0;
}

static int frame_bytes(const gen_t *g)
{
    return g->cfg.channels * (g->cfg.format == PAPP_AUDIO_U8 ? 1 : 2);
}

static void test_mix(int secs)
{
    static gen_t g[NSTREAMS] = {
        { .cfg = { 11025, 1, PAPP_AUDIO_U8 },  .freq = 261.63f },
        { .cfg = { 22050, 1, PAPP_AUDIO_S16 }, .freq = 329.63f },
        { .cfg = { 32000, 2, PAPP_AUDIO_S16 }, .freq = 392.00f },
        { .cfg = { 44100, 2, PAPP_AUDIO_S16 }, .freq = 523.25f },
    };
    for (int i = 0; i < NSTREAMS; i++) {
        g[i].s = papp_audio_open(&g[i].cfg);
        CHECK(g[i].s, "open %lu Hz", (unsigned long)g[i].cfg.rate);
        if (!g[i].s) return;
        generate(&g[i]);
    }

    int64_t t0 = papp_time_us(), t_mark = 0;
    uint64_t mark[NSTREAMS] = { 0 };
    uint32_t under_mark[NSTREAMS] = { 0 };
    while (!papp_exit_requested() && papp_time_us() - t0 < (int64_t)secs * 1000000) {
        for (int i = 0; i < NSTREAMS; i++) {
            size_t n = papp_audio_write(g[i].s, g[i].buf + g[i].off * frame_bytes(&g[i]), g[i].pending, 0);
            g[i].pending -= n;
            g[i].off += n;
            if (!g[i].pending) generate(&g[i]);
        }
        /* Rate measurement window starts after a 1 s warm-up (rings full) */
        if (!t_mark && papp_time_us() - t0 > 1000000) {
            t_mark = papp_time_us();
            for (int i = 0; i < NSTREAMS; i++) {
                papp_audio_stats_t st;
                papp_audio_stats(g[i].s, &st);
                mark[i] = st.played_frames;
                under_mark[i] = st.underruns;
            }
        }
        papp_delay_ms(2);
    }

    int64_t dt = papp_time_us() - t_mark;
    for (int i = 0; i < NSTREAMS; i++) {
        papp_audio_stats_t st;
        papp_audio_stats(g[i].s, &st);
        double rate = (double)(st.played_frames - mark[i]) * 1e6 / (double)dt;
        double err = (rate - g[i].cfg.rate) * 100.0 / g[i].cfg.rate;
        papp_log("%5lu Hz %s %s: measured %.1f Hz (%+.2f%%), queued %lu/%lu, underruns %lu\n",
                 (unsigned long)g[i].cfg.rate, g[i].cfg.channels == 1 ? "mono  " : "stereo",
                 g[i].cfg.format == PAPP_AUDIO_U8 ? "U8 " : "S16", rate, err,
                 (unsigned long)st.queued_frames, (unsigned long)st.capacity_frames,
                 (unsigned long)(st.underruns - under_mark[i]));
        CHECK(fabs(err) < 0.5, "rate off by %.2f%%", err);
        CHECK(st.underruns == under_mark[i], "underruns while fed");
        papp_audio_close(g[i].s);
    }
}

static void test_cycles(int n)
{
    static int16_t zero[256];
    int64_t t0 = papp_time_us();
    for (int i = 0; i < n && !papp_exit_requested(); i++) {
        papp_audio_cfg_t cfg = { .rate = 8000 + (i % 7) * 6000, .channels = 1 + (i & 1),
                                 .format = PAPP_AUDIO_S16, .latency_ms = 20 + (i % 5) * 20 };
        papp_audio_stream_t *s = papp_audio_open(&cfg);
        CHECK(s, "open #%d", i);
        if (!s) return;
        papp_audio_write(s, zero, 64, 0);
        papp_audio_close(s);
    }
    papp_log("%d open/write/close cycles in %lld ms\n", n, (papp_time_us() - t0) / 1000);
}

static void test_block(void)
{
    static int16_t zero[2048];
    papp_audio_cfg_t cfg = { .rate = 16000, .channels = 1, .format = PAPP_AUDIO_S16, .latency_ms = 50 };
    papp_audio_stream_t *s = papp_audio_open(&cfg);
    CHECK(s, "open");
    if (!s) return;
    papp_audio_pause(s, true);
    papp_log("paused; blocking in write() until STOP\n");
    int64_t t0 = papp_time_us();
    size_t n = papp_audio_write(s, zero, 2048, 600000);
    papp_log("write returned %u frames after %lld ms (exit requested: %d)\n", (unsigned)n,
             (papp_time_us() - t0) / 1000, papp_exit_requested());
    /* stream left open on purpose: teardown must close it */
}

int main(int argc, char **argv)
{
    if (!papp_audio_output_rate()) {
        papp_log("no audio\n");
        return 100;
    }
    papp_log("output %lu Hz\n", (unsigned long)papp_audio_output_rate());
    const char *mode = argc > 1 ? argv[1] : "mix";
    int arg = argc > 2 ? atoi(argv[2]) : 0;
    if (!strcmp(mode, "mix")) test_mix(arg ? arg : 6);
    else if (!strcmp(mode, "cycles")) test_cycles(arg ? arg : 1000);
    else if (!strcmp(mode, "block")) test_block();
    else { papp_log("unknown mode %s\n", mode); return 101; }
    papp_log("%s: %d failures\n", mode, s_fail);
    return s_fail;
}
