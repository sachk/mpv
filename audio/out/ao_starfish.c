/*
 * This file is part of mpv.
 */

#include <stdbool.h>

#include "audio/out/ao.h"
#include "audio/out/internal.h"
#include "common/common.h"
#include "common/msg.h"
#include "osdep/timer.h"
#include "video/out/starfish/starfish_ctx.h"

struct priv {
    struct starfish_ctx *ctx;
    bool paused;
    bool playing;
    double last_time;
    double buffered;
    int64_t written_samples;
};

static void wake_ao(void *opaque)
{
    ao_wakeup(opaque);
}

static void drain(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->paused || !p->playing)
        return;

    double now = mp_time_sec();
    if (p->buffered > 0) {
        p->buffered -= (now - p->last_time) * ao->samplerate;
        if (p->buffered < 0)
            p->buffered = 0;
    }
    p->last_time = now;
}

static int init(struct ao *ao)
{
    struct priv *p = ao->priv;

    p->ctx = starfish_ctx_get_current();
    if (!p->ctx) {
        MP_VERBOSE(ao, "No active Starfish context\n");
        return -1;
    }

    if (!starfish_ctx_configure_audio_passthrough(p->ctx, ao->format, ao->samplerate,
                                                  &ao->channels))
    {
        MP_VERBOSE(ao, "Audio format is not supported by Starfish passthrough\n");
        starfish_ctx_unref(p->ctx);
        p->ctx = NULL;
        return -1;
    }

    starfish_ctx_set_wakeup_cb(p->ctx, STARFISH_STREAM_AUDIO, wake_ao, ao);
    ao->device_buffer = ao->samplerate / 4;
    p->last_time = mp_time_sec();
    return 0;
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->ctx) {
        starfish_ctx_set_wakeup_cb(p->ctx, STARFISH_STREAM_AUDIO, NULL, NULL);
        starfish_ctx_unref(p->ctx);
        p->ctx = NULL;
    }
}

static void reset(struct ao *ao)
{
    struct priv *p = ao->priv;

    p->paused = false;
    p->playing = false;
    p->buffered = 0;
    p->written_samples = 0;
    if (p->ctx)
        starfish_ctx_flush(p->ctx, 0);
}

static void start(struct ao *ao)
{
    struct priv *p = ao->priv;

    p->paused = false;
    p->playing = true;
    p->last_time = mp_time_sec();
    if (p->ctx)
        starfish_ctx_resume(p->ctx);
}

static bool set_pause(struct ao *ao, bool paused)
{
    struct priv *p = ao->priv;

    drain(ao);
    p->paused = paused;
    if (p->ctx) {
        if (paused)
            return starfish_ctx_pause(p->ctx);
        p->last_time = mp_time_sec();
        return starfish_ctx_resume(p->ctx);
    }
    return true;
}

static bool audio_write(struct ao *ao, void **data, int samples)
{
    struct priv *p = ao->priv;
    const int64_t pts_ns = p->written_samples * 1000000000LL / ao->samplerate;
    const size_t size = samples * ao->sstride;

    for (int tries = 0; tries < 10; tries++) {
        int r = starfish_ctx_feed_audio(p->ctx, data[0], size, pts_ns);
        if (r == STARFISH_FEED_OK) {
            drain(ao);
            p->written_samples += samples;
            p->buffered += samples;
            return true;
        }
        if (r == STARFISH_FEED_ERROR)
            return false;
        mp_sleep_ns(MP_TIME_MS_TO_NS(10));
    }

    MP_WARN(ao, "Timed out waiting for Starfish audio buffer space\n");
    return false;
}

static void get_state(struct ao *ao, struct mp_pcm_state *state)
{
    struct priv *p = ao->priv;

    drain(ao);
    state->queued_samples = p->buffered;
    state->free_samples = MPMAX(ao->device_buffer - state->queued_samples, 0);
    state->delay = p->buffered / ao->samplerate;
    state->playing = p->playing && p->buffered > 0;
}

const struct ao_driver audio_out_starfish = {
    .description = "LG webOS Starfish",
    .name = "starfish",
    .init = init,
    .uninit = uninit,
    .reset = reset,
    .get_state = get_state,
    .set_pause = set_pause,
    .write = audio_write,
    .start = start,
    .priv_size = sizeof(struct priv),
};
