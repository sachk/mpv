/*
 * This file is part of mpv.
 */

#include <stdbool.h>

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
    AVCodecContext *encoder;
    AVAudioFifo *fifo;
    AVPacket *packet;
    int frame_samples;
    bool paused;
    bool playing;
    double last_time;
    double buffered;
    int64_t written_samples;
};

static void uninit(struct ao *ao);

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
        if (r == STARFISH_FEED_OK)
            return true;
        if (r == STARFISH_FEED_ERROR)
            return false;
        mp_sleep_ns(MP_TIME_MS_TO_NS(10));
    }

    MP_WARN(ao, "Timed out waiting for Starfish audio buffer space\n");
    return false;
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
    p->fifo = av_audio_fifo_alloc(p->encoder->sample_fmt, p->encoder->ch_layout.nb_channels,
                                  p->frame_samples * 4);
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

    starfish_ctx_set_wakeup_cb(p->ctx, STARFISH_STREAM_AUDIO, wake_ao, ao);
    ao->device_buffer = ao->samplerate / 4;
    p->last_time = mp_time_sec();
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
    if (p->fifo)
        av_audio_fifo_drain(p->fifo, av_audio_fifo_size(p->fifo));
    if (p->encoder)
        avcodec_flush_buffers(p->encoder);
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
    if (av_audio_fifo_realloc(p->fifo, av_audio_fifo_size(p->fifo) + samples) < 0)
        return false;
    if (av_audio_fifo_write(p->fifo, data, samples) < samples)
        return false;
    return encode_pending_audio(ao, false);
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
