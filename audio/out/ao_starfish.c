/*
 * This file is part of mpv.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <pthread.h>

#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>

#include "audio/chmap.h"
#include "audio/fmt-conversion.h"
#include "audio/format.h"
#include "audio/out/ao.h"
#include "audio/out/internal.h"
#include "common/common.h"
#include "common/msg.h"
#include "osdep/timer.h"
#include "video/out/starfish/starfish_ctx.h"

struct priv {
    struct starfish_ctx *ctx;
    pthread_mutex_t lock;
    bool lock_initialized;
    AVCodecContext *encoder;
    AVAudioFifo *fifo;
    AVPacket *packet;
    int frame_samples;
    bool paused;
    bool playing;
    double last_time;
    double buffered;
    int latency_samples;
    int outburst;
    int64_t written_samples;
    bool primed;
    bool logged_write;
    bool needs_sync;
};

#define STARFISH_AUDIO_TARGET_LATENCY_SEC 1.0
#define STARFISH_AUDIO_BUFFER_SEC 3.0
#define STARFISH_AUDIO_START_PRIME_FRAMES 4
#define STARFISH_AUDIO_SEEK_PREROLL_SEC 4.0

static void uninit(struct ao *ao);
static int encode_silence_frame(struct ao *ao, int samples);
static bool audio_prime_cb(void *opaque, int64_t pts_ns);

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

static enum AVSampleFormat select_encoder_format(const AVCodec *codec)
{
    if (!codec || !codec->sample_fmts)
        return AV_SAMPLE_FMT_FLTP;

    for (const enum AVSampleFormat *fmt = codec->sample_fmts; *fmt != AV_SAMPLE_FMT_NONE; fmt++) {
        if (*fmt == AV_SAMPLE_FMT_FLTP)
            return *fmt;
    }

    return codec->sample_fmts[0];
}

static bool feed_encoded_packet(struct ao *ao, const uint8_t *data, size_t size, int64_t pts_ns)
{
    struct priv *p = ao->priv;

    for (int tries = 0; tries < 10; tries++) {
        int r = starfish_ctx_feed_audio(p->ctx, data, size, pts_ns);
        if (r != STARFISH_FEED_AGAIN)
            MP_TRACE(ao, "ao_starfish feed_encoded_packet ctx=%p size=%zu pts=%" PRId64
                     " try=%d result=%d\n",
                     p->ctx, size, pts_ns, tries + 1, r);
        if (r == STARFISH_FEED_OK)
            return true;
        if (r == STARFISH_FEED_ERROR)
            return false;
        mp_sleep_ns(MP_TIME_MS_TO_NS(10));
    }

    MP_WARN(ao, "Timed out waiting for Starfish audio buffer space\n");
    return false;
}

static bool sync_written_samples_to_seek_target(struct ao *ao, bool log_missing)
{
    struct priv *p = ao->priv;
    int64_t seek_target_ns = 0;

    if (!p->ctx || !p->encoder)
        return false;
    if (!starfish_ctx_get_seek_target_ns(p->ctx, &seek_target_ns) || seek_target_ns <= 0) {
        if (log_missing) {
            MP_INFO(ao, "ao_starfish sync: no valid seek target (keeping samples=%" PRId64 ")\n",
                    p->written_samples);
        }
        return false;
    }

    p->written_samples = av_rescale_q(seek_target_ns, (AVRational){1, 1000000000},
                                      p->encoder->time_base);
    MP_INFO(ao, "ao_starfish synced audio pts base to seek target ns=%" PRId64
            " samples=%" PRId64 "\n",
            seek_target_ns, p->written_samples);
    return true;
}

static bool prime_silence_frames(struct ao *ao, int frames, const char *reason)
{
    struct priv *p = ao->priv;

    if (frames <= 0)
        return true;

    int primed_packets = 0;
    int primed_frames = 0;
    for (int n = 0; n < frames; n++) {
        int ret = encode_silence_frame(ao, p->frame_samples);
        if (ret < 0) {
            MP_WARN(ao, "Failed to %s Starfish audio with AAC silence\n",
                    reason ? reason : "prime");
            return false;
        }
        primed_frames++;
        primed_packets += ret;
    }

    if (primed_packets > 0) {
        MP_INFO(ao,
                "%s Starfish audio with %d silent samples across %d frames (%d packets)\n",
                reason ? reason : "Primed", primed_frames * p->frame_samples,
                primed_frames, primed_packets);
    } else {
        MP_WARN(ao, "AAC %s produced no output packets after %d silent frames\n",
                reason ? reason : "prime", primed_frames);
    }

    return true;
}

static bool ensure_audio_primed(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->primed)
        return true;
    if (!prime_silence_frames(ao, STARFISH_AUDIO_START_PRIME_FRAMES,
                              "Pre-primed"))
        return false;

    p->primed = true;
    return true;
}

static bool prime_at_ns_locked(struct ao *ao, int64_t pts_ns, const char *reason)
{
    struct priv *p = ao->priv;

    if (!p->encoder || pts_ns < 0)
        return false;

    av_audio_fifo_drain(p->fifo, av_audio_fifo_size(p->fifo));
    avcodec_flush_buffers(p->encoder);
    p->written_samples = av_rescale_q(pts_ns, (AVRational){1, 1000000000},
                                      p->encoder->time_base);
    p->primed = false;
    p->needs_sync = false;
    p->buffered = 0;
    MP_INFO(ao, "ao_starfish prime at %s pts=%" PRId64 " samples=%" PRId64 "\n",
            reason ? reason : "request", pts_ns, p->written_samples);

    int seek_frames = (int)((ao->samplerate * STARFISH_AUDIO_SEEK_PREROLL_SEC +
                             p->frame_samples - 1) / p->frame_samples);
    if (!prime_silence_frames(ao, seek_frames, "Seek-prerolled"))
        return false;
    p->primed = true;
    return true;
}

static bool audio_prime_cb(void *opaque, int64_t pts_ns)
{
    struct ao *ao = opaque;
    struct priv *p = ao ? ao->priv : NULL;
    bool ok = false;

    if (!p || !p->lock_initialized)
        return false;

    pthread_mutex_lock(&p->lock);
    ok = prime_at_ns_locked(ao, pts_ns, "starfish segment");
    pthread_mutex_unlock(&p->lock);
    return ok;
}

static bool encode_pending_audio(struct ao *ao, bool flush_tail)
{
    struct priv *p = ao->priv;

    while (av_audio_fifo_size(p->fifo) >= p->frame_samples ||
           (flush_tail && av_audio_fifo_size(p->fifo) > 0))
    {
        AVFrame *frame = av_frame_alloc();
        if (!frame)
            return false;

        frame->nb_samples = p->frame_samples;
        frame->format = p->encoder->sample_fmt;
        frame->sample_rate = p->encoder->sample_rate;
        if (av_channel_layout_copy(&frame->ch_layout, &p->encoder->ch_layout) < 0) {
            av_frame_free(&frame);
            return false;
        }
        if (av_frame_get_buffer(frame, 0) < 0 || av_frame_make_writable(frame) < 0) {
            av_frame_free(&frame);
            return false;
        }

        int available = av_audio_fifo_size(p->fifo);
        if (available < p->frame_samples) {
            av_samples_set_silence(frame->extended_data, 0, p->frame_samples,
                                   p->encoder->ch_layout.nb_channels,
                                   p->encoder->sample_fmt);
            if (av_audio_fifo_read(p->fifo, (void **)frame->extended_data, available) < available) {
                av_frame_free(&frame);
                return false;
            }
        } else if (av_audio_fifo_read(p->fifo, (void **)frame->extended_data,
                                      p->frame_samples) < p->frame_samples) {
            av_frame_free(&frame);
            return false;
        }

        frame->pts = p->written_samples;
        p->written_samples += p->frame_samples;

        if (avcodec_send_frame(p->encoder, frame) < 0) {
            av_frame_free(&frame);
            return false;
        }
        av_frame_free(&frame);

        for (;;) {
            int ret = avcodec_receive_packet(p->encoder, p->packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                return false;

            int64_t pts_ns = 0;
            if (p->packet->pts != AV_NOPTS_VALUE)
                pts_ns = av_rescale_q(p->packet->pts, p->encoder->time_base,
                                      (AVRational){1, 1000000000});
            if (pts_ns < 0)
                pts_ns = 0;

            if (!feed_encoded_packet(ao, p->packet->data, p->packet->size, pts_ns)) {
                av_packet_unref(p->packet);
                return false;
            }

            drain(ao);
            p->buffered += p->frame_samples;
            av_packet_unref(p->packet);
        }
    }

    return true;
}

static int encode_silence_frame(struct ao *ao, int samples)
{
    struct priv *p = ao->priv;
    int packets = 0;

    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return -1;

    frame->nb_samples = samples;
    frame->format = p->encoder->sample_fmt;
    frame->sample_rate = p->encoder->sample_rate;
    if (av_channel_layout_copy(&frame->ch_layout, &p->encoder->ch_layout) < 0) {
        av_frame_free(&frame);
        return -1;
    }
    if (av_frame_get_buffer(frame, 0) < 0 || av_frame_make_writable(frame) < 0) {
        av_frame_free(&frame);
        return -1;
    }

    av_samples_set_silence(frame->extended_data, 0, samples,
                           p->encoder->ch_layout.nb_channels,
                           p->encoder->sample_fmt);

    frame->pts = p->written_samples;
    p->written_samples += samples;

    if (avcodec_send_frame(p->encoder, frame) < 0) {
        av_frame_free(&frame);
        return -1;
    }
    av_frame_free(&frame);

    for (;;) {
        int ret = avcodec_receive_packet(p->encoder, p->packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            return -1;

        int64_t pts_ns = 0;
        if (p->packet->pts != AV_NOPTS_VALUE)
            pts_ns = av_rescale_q(p->packet->pts, p->encoder->time_base,
                                  (AVRational){1, 1000000000});
        if (pts_ns < 0)
            pts_ns = 0;

        if (!feed_encoded_packet(ao, p->packet->data, p->packet->size, pts_ns)) {
            av_packet_unref(p->packet);
            return -1;
        }

        drain(ao);
        p->buffered += samples;
        packets++;
        av_packet_unref(p->packet);
    }

    return packets;
}

static int init(struct ao *ao)
{
    struct priv *p = ao->priv;
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    enum AVSampleFormat sample_fmt;
    int mp_format;

    p->ctx = starfish_ctx_get_current();
    if (!p->ctx) {
        MP_VERBOSE(ao, "No active Starfish context\n");
        return -1;
    }
    MP_INFO(ao, "ao_starfish init ctx=%p\n", p->ctx);
    if (pthread_mutex_init(&p->lock, NULL) != 0) {
        starfish_ctx_unref(p->ctx);
        p->ctx = NULL;
        return -1;
    }
    p->lock_initialized = true;

    if (!codec) {
        MP_VERBOSE(ao, "AAC encoder is not available\n");
        starfish_ctx_unref(p->ctx);
        p->ctx = NULL;
        return -1;
    }

    sample_fmt = select_encoder_format(codec);
    mp_format = af_from_avformat(sample_fmt);
    if (mp_format == AF_FORMAT_UNKNOWN) {
        MP_VERBOSE(ao, "No mpv audio format for AAC encoder sample format %d\n", sample_fmt);
        starfish_ctx_unref(p->ctx);
        p->ctx = NULL;
        return -1;
    }

    ao->samplerate = 48000;
    ao->channels = (struct mp_chmap)MP_CHMAP_INIT_STEREO;
    ao->format = mp_format;

    p->encoder = avcodec_alloc_context3(codec);
    p->packet = av_packet_alloc();
    if (!p->encoder || !p->packet) {
        uninit(ao);
        return -1;
    }

    p->encoder->sample_fmt = sample_fmt;
    p->encoder->sample_rate = ao->samplerate;
    p->encoder->time_base = (AVRational){1, ao->samplerate};
    p->encoder->bit_rate = 192000;
    p->encoder->profile = AV_PROFILE_AAC_LOW;
    av_channel_layout_default(&p->encoder->ch_layout, ao->channels.num);

    if (avcodec_open2(p->encoder, codec, NULL) < 0) {
        MP_ERR(ao, "Failed to open AAC encoder\n");
        uninit(ao);
        return -1;
    }

    p->frame_samples = p->encoder->frame_size > 0 ? p->encoder->frame_size : 1024;
    p->outburst = p->frame_samples;
    p->latency_samples = ao->samplerate * STARFISH_AUDIO_TARGET_LATENCY_SEC;
    p->fifo = av_audio_fifo_alloc(p->encoder->sample_fmt, p->encoder->ch_layout.nb_channels,
                                  ao->samplerate * STARFISH_AUDIO_BUFFER_SEC);
    if (!p->fifo) {
        MP_ERR(ao, "Failed to allocate AAC FIFO\n");
        uninit(ao);
        return -1;
    }

    if (!starfish_ctx_configure_audio_aac(p->ctx, ao->channels.num, ao->samplerate,
                                          AV_PROFILE_AAC_LOW, true)) {
        MP_VERBOSE(ao, "Failed to configure Starfish AAC audio\n");
        uninit(ao);
        return -1;
    }

    MP_INFO(ao, "ao_starfish init samplerate=%d channels=%d frame_samples=%d\n",
            ao->samplerate, ao->channels.num, p->frame_samples);
    starfish_ctx_set_wakeup_cb(p->ctx, STARFISH_STREAM_AUDIO, wake_ao, ao);
    starfish_ctx_set_audio_prime_cb(p->ctx, audio_prime_cb, ao);
    sync_written_samples_to_seek_target(ao, true);
    if (!ensure_audio_primed(ao)) {
        uninit(ao);
        return -1;
    }
    ao->device_buffer = p->latency_samples +
                        ao->samplerate * STARFISH_AUDIO_BUFFER_SEC;
    p->last_time = mp_time_sec();
    MP_INFO(ao, "ao_starfish buffering latency=%d device_buffer=%d\n",
            p->latency_samples, ao->device_buffer);
    return 0;
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->packet) {
        av_packet_free(&p->packet);
        p->packet = NULL;
    }
    if (p->fifo) {
        av_audio_fifo_free(p->fifo);
        p->fifo = NULL;
    }
    if (p->encoder) {
        avcodec_free_context(&p->encoder);
        p->encoder = NULL;
    }
    if (p->ctx) {
        starfish_ctx_set_wakeup_cb(p->ctx, STARFISH_STREAM_AUDIO, NULL, NULL);
        starfish_ctx_set_audio_prime_cb(p->ctx, NULL, NULL);
        starfish_ctx_unref(p->ctx);
        p->ctx = NULL;
    }
    if (p->lock_initialized) {
        pthread_mutex_destroy(&p->lock);
        p->lock_initialized = false;
    }
}

static void reset(struct ao *ao)
{
    struct priv *p = ao->priv;

    pthread_mutex_lock(&p->lock);
    p->paused = false;
    p->playing = false;
    p->buffered = 0;
    p->primed = false;
    p->logged_write = false;
    p->written_samples = 0;
    if (p->fifo)
        av_audio_fifo_drain(p->fifo, av_audio_fifo_size(p->fifo));
    if (p->encoder)
        avcodec_flush_buffers(p->encoder);
    p->needs_sync = true;
    pthread_mutex_unlock(&p->lock);
    if (p->ctx)
        ao_wakeup(ao);
}

static void start(struct ao *ao)
{
    struct priv *p = ao->priv;

    pthread_mutex_lock(&p->lock);
    p->paused = false;
    p->playing = true;
    p->last_time = mp_time_sec();
    MP_INFO(ao, "ao_starfish start\n");
    if (p->needs_sync) {
        if (sync_written_samples_to_seek_target(ao, true))
            p->needs_sync = false;
        else
            MP_INFO(ao, "ao_starfish start deferred audio prime until seek target is known\n");
    }
    if (!p->needs_sync && !ensure_audio_primed(ao))
        MP_WARN(ao, "Unable to re-prime Starfish audio on start\n");
    pthread_mutex_unlock(&p->lock);
    if (p->ctx)
        starfish_ctx_resume(p->ctx);
}

static bool set_pause(struct ao *ao, bool paused)
{
    struct priv *p = ao->priv;

    pthread_mutex_lock(&p->lock);
    drain(ao);
    p->paused = paused;
    pthread_mutex_unlock(&p->lock);
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
    bool ok = false;

    pthread_mutex_lock(&p->lock);
    if (p->needs_sync) {
        int64_t target_ns = 0;
        if (starfish_ctx_get_seek_target_ns(p->ctx, &target_ns)) {
            p->written_samples = av_rescale_q(target_ns, (AVRational){1, 1000000000},
                                              p->encoder->time_base);
            MP_INFO(ao, "ao_starfish synced audio pts base to seek target ns=%" PRId64
                    " samples=%" PRId64 "\n",
                    target_ns, p->written_samples);
            p->needs_sync = false;
        } else {
            /* If we don't have a seek target yet, we should probably wait or
             * at least not clear needs_sync so we try again on next write. */
            MP_WARN(ao, "ao_starfish sync: waiting for seek target...\n");
        }
    }
    if (!p->logged_write) {
        MP_INFO(ao, "ao_starfish first write samples=%d\n", samples);
        p->logged_write = true;
    }
    if (!ensure_audio_primed(ao))
        goto done;
    if (av_audio_fifo_realloc(p->fifo, av_audio_fifo_size(p->fifo) + samples) < 0)
        goto done;
    if (av_audio_fifo_write(p->fifo, data, samples) < samples)
        goto done;
    if (!encode_pending_audio(ao, false))
        goto done;
    if (p->buffered <= 0)
        p->buffered = p->latency_samples;
    ok = true;

done:
    pthread_mutex_unlock(&p->lock);
    return ok;
}

static void get_state(struct ao *ao, struct mp_pcm_state *state)
{
    struct priv *p = ao->priv;
    int queued_fifo = 0;
    double queued_total;

    pthread_mutex_lock(&p->lock);
    queued_fifo = p->fifo ? av_audio_fifo_size(p->fifo) : 0;
    if (p->needs_sync && sync_written_samples_to_seek_target(ao, false)) {
        p->needs_sync = false;
        if (!ensure_audio_primed(ao))
            MP_WARN(ao, "Unable to re-prime Starfish audio from get_state\n");
        queued_fifo = p->fifo ? av_audio_fifo_size(p->fifo) : 0;
    }

    drain(ao);
    queued_total = p->buffered + queued_fifo;
    state->queued_samples = queued_total;
    state->free_samples = MPMAX(ao->device_buffer - p->latency_samples -
                                state->queued_samples, 0);
    state->free_samples = state->free_samples / p->outburst * p->outburst;
    state->delay = queued_total;
    if (state->delay < p->latency_samples)
        state->delay = p->latency_samples;
    state->delay /= ao->samplerate;
    state->playing = p->playing && p->buffered > 0;
    pthread_mutex_unlock(&p->lock);
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
