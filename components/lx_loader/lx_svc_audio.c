/*
 * AUDI interface: app streams on top of the mixer (lx_audio). Streams are
 * owned by the mixer; teardown closes whatever is left. A blocked write gives
 * up as soon as the launcher asks the app to exit, so STOP never waits on it.
 */
#include "lx_services.h"
#include "lx_audio.h"

static uint32_t audi_output_rate(void)
{
    return lx_audio_output_rate();
}

static papp_audio_stream_t *audi_open(const papp_audio_cfg_t *cfg)
{
    if (!cfg) return NULL;
    SVC_ENTER();
    lx_audio_stream_t *s = lx_audio_open(cfg->rate, cfg->channels, (lx_audio_fmt_t)cfg->format,
                                         cfg->latency_ms);
    SVC_LEAVE();
    return (papp_audio_stream_t *)s;
}

static void audi_close(papp_audio_stream_t *s)
{
    SVC_ENTER();
    lx_audio_close((lx_audio_stream_t *)s);
    SVC_LEAVE();
}

static size_t audi_write(papp_audio_stream_t *s, const void *data, size_t frames, uint32_t timeout_ms)
{
    SVC_ENTER();
    size_t n = lx_audio_write((lx_audio_stream_t *)s, data, frames, timeout_ms, &lx_svc_exit_flag);
    SVC_LEAVE();
    return n;
}

static void audi_pause(papp_audio_stream_t *s, bool pause)
{
    lx_audio_pause((lx_audio_stream_t *)s, pause);
}

static void audi_set_gain(papp_audio_stream_t *s, int percent)
{
    lx_audio_set_gain((lx_audio_stream_t *)s, percent);
}

static void audi_clear(papp_audio_stream_t *s)
{
    SVC_ENTER();
    lx_audio_clear((lx_audio_stream_t *)s);
    SVC_LEAVE();
}

static void audi_stats(papp_audio_stream_t *s, papp_audio_stats_t *out)
{
    lx_audio_stats_t st;
    lx_audio_stats((lx_audio_stream_t *)s, &st);
    *out = (papp_audio_stats_t){
        .queued_frames = st.queued_frames, .capacity_frames = st.capacity_frames,
        .underruns = st.underruns, .played_frames = st.played_frames,
    };
}

const papp_if_audi_t lx_if_audi = {
    .hdr = { PAPP_IF_AUDI, PAPP_IF_AUDI_VER, sizeof(papp_if_audi_t) },
    .output_rate = audi_output_rate,
    .open        = audi_open,
    .close       = audi_close,
    .write       = audi_write,
    .pause       = audi_pause,
    .set_gain    = audi_set_gain,
    .clear       = audi_clear,
    .stats       = audi_stats,
};
