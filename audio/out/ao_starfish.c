/*
 * This file is part of mpv.
 */

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/mem.h>

#include "audio/chmap.h"
#include "audio/fmt-conversion.h"
#include "audio/format.h"
#include "audio/out/ao.h"
#include "audio/out/internal.h"
#include "common/common.h"
#include "common/msg.h"
#include "options/m_config_core.h"
#include "options/options.h"
#include "osdep/timer.h"
#include "video/out/starfish/starfish_ctx.h"

struct encoded_packet {
    struct encoded_packet *next;
    uint8_t *data;
    size_t size;
    int64_t pts_ns;
    int samples;
};

struct priv {
    struct starfish_ctx *ctx;
    struct m_config_cache *opts_cache;
    struct MPOpts *opts;
    pthread_mutex_t lock;
    bool lock_initialized;
    AVCodecContext *encoder;
    AVAudioFifo *fifo;
    AVPacket *packet;
    int frame_samples;
    bool paused;
    bool playing;
    double last_time;
    double buffered_samples;
    int latency_samples;
    int outburst;
    int64_t written_samples;
    bool primed;
    bool logged_write;
    bool needs_sync;
    bool logged_audio_delay;
    double last_audio_delay;
    struct encoded_packet *pending_head;
    struct encoded_packet *pending_tail;
    int pending_samples;
    bool feed_blocked;
    /* PCM mode: feed raw interleaved PCM instead of AAC. When set, the
     * encoder/fifo above are unused (NULL). bytes_per_frame is the size of one
     * interleaved sample-frame (channels * bytes-per-sample). */
    bool pcm_mode;
    int bytes_per_frame;
};

#define STARFISH_AUDIO_TARGET_LATENCY_SEC 0.08
// Conduit buffer between mpv and Starfish only. The real play-ahead lives in
// the Starfish ES queue (~1.6s, MAX_FEED_AHEAD_NS). A large value here just
// makes mpv dump one giant write() at startup (a ~3s AAC encode spike causing
// stutter) and coarsens the refill cadence, so keep it small.
#define STARFISH_AUDIO_BUFFER_SEC 0.5
#define STARFISH_AUDIO_START_PRIME_FRAMES 3

// PCM mode: decoded audio is fed to Starfish as a raw-PCM elementary stream
// instead of being re-encoded to AAC. Kept as a distinct path so the legacy
// AAC encode code below can be removed wholesale once PCM is the only mode.
#define STARFISH_PCM_FORMAT AF_FORMAT_S16
#define STARFISH_PCM_BITS_PER_SAMPLE 16
#define STARFISH_PCM_FORMAT_TOKEN "S16LE"

static void uninit(struct ao *ao);
static int encode_silence_frame(struct ao *ao, int samples);
static bool prime_at_ns_locked(struct ao *ao, int64_t pts_ns, const char *reason);
static bool prime_pcm_silence(struct ao *ao, int frames, const char *reason);
static bool audio_prime_cb(void *opaque, int64_t pts_ns);
static enum AVSampleFormat select_encoder_format(const AVCodec *codec);

// Audio PTS time base is the same for both modes (sample-accurate at the
// output rate); avoids reaching into p->encoder, which is NULL in PCM mode.
static inline AVRational audio_time_base(struct ao *ao)
{
    return (AVRational){1, ao->samplerate};
}

static bool env_wants_pcm_audio(void)
{
    const char *codec = getenv("STARFISH_AUDIO_CODEC");
    return codec && (strcmp(codec, "pcm") == 0 || strcmp(codec, "PCM") == 0);
}

static double current_audio_delay(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (!p->opts_cache || !p->opts)
        return 0.0;

    m_config_cache_update(p->opts_cache);
    return p->opts->audio_delay;
}

static int64_t apply_audio_delay_to_pts(struct ao *ao, int64_t pts_ns)
{
    struct priv *p = ao->priv;
    const double delay = current_audio_delay(ao);
    const int64_t delayed_pts_ns =
        pts_ns + (int64_t)llround(delay * 1000000000.0);

    if (!p->logged_audio_delay || fabs(delay - p->last_audio_delay) >= 0.0005) {
        MP_INFO(ao, "ao_starfish audio-delay applied delay=%.3f base_pts=%.3f feed_pts=%.3f\n",
                delay, (double)pts_ns / 1000000000.0,
                (double)MPMAX(delayed_pts_ns, 0) / 1000000000.0);
        p->logged_audio_delay = true;
        p->last_audio_delay = delay;
    }

    return MPMAX(delayed_pts_ns, 0);
}

static bool reopen_encoder_locked(struct ao *ao)
{
    struct priv *p = ao->priv;
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    enum AVSampleFormat sample_fmt;

    if (!codec)
        return false;

    sample_fmt = p->encoder ? p->encoder->sample_fmt : select_encoder_format(codec);
    avcodec_free_context(&p->encoder);
    p->encoder = avcodec_alloc_context3(codec);
    if (!p->encoder)
        return false;

    p->encoder->sample_fmt = sample_fmt;
    p->encoder->sample_rate = ao->samplerate;
    p->encoder->time_base = (AVRational){1, ao->samplerate};
    p->encoder->bit_rate = 192000;
    p->encoder->profile = AV_PROFILE_AAC_LOW;
    av_channel_layout_default(&p->encoder->ch_layout, ao->channels.num);

    if (avcodec_open2(p->encoder, codec, NULL) < 0) {
        MP_ERR(ao, "Failed to reopen AAC encoder\n");
        avcodec_free_context(&p->encoder);
        return false;
    }

    p->frame_samples = p->encoder->frame_size > 0 ? p->encoder->frame_size : 1024;
    p->outburst = p->frame_samples;
    p->latency_samples = ao->samplerate * STARFISH_AUDIO_TARGET_LATENCY_SEC;
    return true;
}

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
    if (p->buffered_samples > 0) {
        p->buffered_samples -= (now - p->last_time) * ao->samplerate;
        if (p->buffered_samples < 0)
            p->buffered_samples = 0;
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

static void free_encoded_packet(struct encoded_packet *pkt)
{
    if (!pkt)
        return;
    av_free(pkt->data);
    av_free(pkt);
}

static void free_pending_packets_locked(struct priv *p)
{
    while (p->pending_head) {
        struct encoded_packet *pkt = p->pending_head;
        p->pending_head = pkt->next;
        free_encoded_packet(pkt);
    }
    p->pending_tail = NULL;
    p->pending_samples = 0;
    p->feed_blocked = false;
}

static bool queue_encoded_packet_locked(struct ao *ao, const uint8_t *data,
                                        size_t size, int64_t pts_ns,
                                        int samples)
{
    struct priv *p = ao->priv;
    struct encoded_packet *pkt = av_mallocz(sizeof(*pkt));

    if (!pkt)
        return false;
    pkt->data = av_memdup(data, size);
    if (!pkt->data) {
        av_free(pkt);
        return false;
    }
    pkt->size = size;
    pkt->pts_ns = pts_ns;
    pkt->samples = samples;

    if (p->pending_tail) {
        p->pending_tail->next = pkt;
    } else {
        p->pending_head = pkt;
    }
    p->pending_tail = pkt;
    p->pending_samples += samples;
    p->feed_blocked = false;
    return true;
}

static bool feed_pending_packets(struct ao *ao)
{
    struct priv *p = ao->priv;
    bool ok = true;

    for (;;) {
        pthread_mutex_lock(&p->lock);
        struct encoded_packet *pkt = p->pending_head;
        if (!pkt || !p->ctx) {
            p->feed_blocked = false;
            pthread_mutex_unlock(&p->lock);
            break;
        }
        p->pending_head = pkt->next;
        if (p->pending_tail == pkt)
            p->pending_tail = NULL;
        pkt->next = NULL;
        p->pending_samples = MPMAX(p->pending_samples - pkt->samples, 0);
        struct starfish_ctx *ctx = starfish_ctx_retain(p->ctx);
        pthread_mutex_unlock(&p->lock);

        int r = starfish_ctx_feed_audio(ctx, pkt->data, pkt->size, pkt->pts_ns);
        starfish_ctx_unref(ctx);
        if (r == STARFISH_FEED_AGAIN) {
            pthread_mutex_lock(&p->lock);
            pkt->next = p->pending_head;
            p->pending_head = pkt;
            if (!p->pending_tail)
                p->pending_tail = pkt;
            p->pending_samples += pkt->samples;
            p->feed_blocked = true;
            pthread_mutex_unlock(&p->lock);
            break;
        }
        if (r == STARFISH_FEED_ERROR) {
            MP_WARN(ao, "Starfish rejected encoded audio packet pts=%" PRId64
                    " size=%zu\n", pkt->pts_ns, pkt->size);
            ok = false;
        }

        MP_TRACE(ao, "ao_starfish feed packet size=%zu pts=%" PRId64
                 " result=%d\n", pkt->size, pkt->pts_ns, r);

        pthread_mutex_lock(&p->lock);
        if (r == STARFISH_FEED_OK) {
            drain(ao);
            p->buffered_samples += pkt->samples;
        }
        pthread_mutex_unlock(&p->lock);
        free_encoded_packet(pkt);

        if (!ok)
            break;
    }

    return ok;
}

static bool sync_written_samples_to_seek_target(struct ao *ao, bool log_missing)
{
    struct priv *p = ao->priv;
    int64_t seek_target_ns = 0;

    if (!p->ctx || (!p->pcm_mode && !p->encoder))
        return false;
    if (!starfish_ctx_get_seek_target_ns(p->ctx, &seek_target_ns) || seek_target_ns <= 0) {
        if (log_missing) {
            MP_INFO(ao, "ao_starfish sync: no valid seek target (keeping samples=%" PRId64 ")\n",
                    p->written_samples);
        }
        return false;
    }

    p->written_samples = av_rescale_q(seek_target_ns, (AVRational){1, 1000000000},
                                      audio_time_base(ao));
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

    if (primed_packets <= 0) {
        MP_WARN(ao, "AAC %s produced no output packets after %d silent frames\n",
                reason ? reason : "prime", primed_frames);
        return false;
    }

    MP_INFO(ao,
            "%s Starfish audio with %d silent samples across %d frames (%d packets)\n",
            reason ? reason : "Primed", primed_frames * p->frame_samples,
            primed_frames, primed_packets);
    return true;
}

// PCM counterpart of prime_silence_frames: queue zeroed interleaved PCM so the
// pipeline has a little audio ahead of the first real samples. Lock held.
static bool prime_pcm_silence(struct ao *ao, int frames, const char *reason)
{
    struct priv *p = ao->priv;

    if (frames <= 0)
        return true;

    const int total_samples = frames * p->frame_samples;
    const size_t bytes = (size_t)total_samples * p->bytes_per_frame;
    uint8_t *silence = av_mallocz(bytes);
    if (!silence) {
        MP_WARN(ao, "Failed to allocate PCM silence for %s\n",
                reason ? reason : "prime");
        return false;
    }

    int64_t pts_ns = av_rescale_q(p->written_samples, audio_time_base(ao),
                                  (AVRational){1, 1000000000});
    if (pts_ns < 0)
        pts_ns = 0;
    p->written_samples += total_samples;

    bool ok = queue_encoded_packet_locked(ao, silence, bytes,
                                          apply_audio_delay_to_pts(ao, pts_ns),
                                          total_samples);
    av_free(silence);
    if (ok) {
        MP_INFO(ao, "%s Starfish audio with %d silent PCM samples\n",
                reason ? reason : "Primed", total_samples);
    } else {
        MP_WARN(ao, "Failed to %s Starfish audio with PCM silence\n",
                reason ? reason : "prime");
    }
    return ok;
}

static bool ensure_audio_primed(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->primed)
        return true;
    if (p->pcm_mode) {
        if (!prime_pcm_silence(ao, STARFISH_AUDIO_START_PRIME_FRAMES,
                               "Pre-primed"))
            return false;
    } else if (!prime_silence_frames(ao, STARFISH_AUDIO_START_PRIME_FRAMES,
                                     "Pre-primed")) {
        return false;
    }

    p->primed = true;
    return true;
}

static bool prime_at_ns_locked(struct ao *ao, int64_t pts_ns, const char *reason)
{
    struct priv *p = ao->priv;

    if (pts_ns < 0 || (!p->pcm_mode && !p->encoder))
        return false;

    free_pending_packets_locked(p);
    if (!p->pcm_mode) {
        av_audio_fifo_drain(p->fifo, av_audio_fifo_size(p->fifo));
        if (!reopen_encoder_locked(ao))
            return false;
    }
    p->written_samples = av_rescale_q(pts_ns, (AVRational){1, 1000000000},
                                      audio_time_base(ao));
    p->primed = false;
    p->needs_sync = false;
    p->buffered_samples = 0;
    MP_INFO(ao, "ao_starfish prime at %s pts=%" PRId64 " samples=%" PRId64 "\n",
            reason ? reason : "request", pts_ns, p->written_samples);
    return ensure_audio_primed(ao);
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
    if (ok)
        ok = feed_pending_packets(ao);
    if (ok)
        ao_wakeup(ao);
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

            if (!queue_encoded_packet_locked(ao, p->packet->data,
                                             p->packet->size,
                                             apply_audio_delay_to_pts(ao, pts_ns),
                                             p->frame_samples)) {
                av_packet_unref(p->packet);
                return false;
            }

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

        if (!queue_encoded_packet_locked(ao, p->packet->data, p->packet->size,
                                         apply_audio_delay_to_pts(ao, pts_ns),
                                         samples)) {
            av_packet_unref(p->packet);
            return -1;
        }

        packets++;
        av_packet_unref(p->packet);
    }

    return packets;
}

static int init(struct ao *ao)
{
    struct priv *p = ao->priv;

    p->ctx = starfish_ctx_get_current();
    if (!p->ctx) {
        MP_VERBOSE(ao, "No active Starfish context\n");
        return -1;
    }
    MP_INFO(ao, "ao_starfish init ctx=%p\n", p->ctx);
    p->opts_cache = m_config_cache_alloc(ao, ao->global, &mp_opt_root);
    p->opts = p->opts_cache ? p->opts_cache->opts : NULL;
    if (pthread_mutex_init(&p->lock, NULL) != 0) {
        starfish_ctx_unref(p->ctx);
        p->ctx = NULL;
        return -1;
    }
    p->lock_initialized = true;

    p->pcm_mode = env_wants_pcm_audio();

    if (p->pcm_mode) {
        // Decode-to-PCM path: advertise interleaved S16 so mpv's filter chain
        // delivers exactly what the Starfish PCM sink wants; we then hand those
        // buffers straight to the audio ES (esData=2). No encoder/FIFO.
        ao->samplerate = 48000;
        ao->channels = (struct mp_chmap)MP_CHMAP_INIT_STEREO;
        ao->format = STARFISH_PCM_FORMAT;
        p->bytes_per_frame = ao->channels.num * (STARFISH_PCM_BITS_PER_SAMPLE / 8);
        p->frame_samples = 1024;
        p->outburst = p->frame_samples;
        p->latency_samples = ao->samplerate * STARFISH_AUDIO_TARGET_LATENCY_SEC;

        if (!starfish_ctx_configure_audio_pcm(p->ctx, ao->channels.num,
                                              ao->samplerate,
                                              STARFISH_PCM_BITS_PER_SAMPLE,
                                              STARFISH_PCM_FORMAT_TOKEN)) {
            MP_VERBOSE(ao, "Failed to configure Starfish PCM audio\n");
            uninit(ao);
            return -1;
        }
    } else {
        // ---- Legacy AAC encode path (removable once PCM is the only mode) ----
        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        enum AVSampleFormat sample_fmt;
        int mp_format;

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
    }

    MP_INFO(ao, "ao_starfish init mode=%s samplerate=%d channels=%d frame_samples=%d\n",
            p->pcm_mode ? "pcm" : "aac", ao->samplerate, ao->channels.num,
            p->frame_samples);
    starfish_ctx_set_wakeup_cb(p->ctx, STARFISH_STREAM_AUDIO, wake_ao, ao);
    starfish_ctx_set_audio_prime_cb(p->ctx, audio_prime_cb, ao);
    pthread_mutex_lock(&p->lock);
    int64_t start_target_ns = 0;
    bool needs_segment_prime = false;
    if (starfish_ctx_get_audio_reset_target_ns(p->ctx, &start_target_ns,
                                               &needs_segment_prime)) {
        if (needs_segment_prime) {
            p->needs_sync = true;
            MP_INFO(ao, "ao_starfish init waiting for segment prime target=%.3f\n",
                    (double)start_target_ns / 1000000000.0);
        } else if (!prime_at_ns_locked(ao, start_target_ns, "audio init")) {
            pthread_mutex_unlock(&p->lock);
            uninit(ao);
            return -1;
        }
    } else {
        sync_written_samples_to_seek_target(ao, true);
    }
    if (!p->needs_sync && !ensure_audio_primed(ao)) {
        pthread_mutex_unlock(&p->lock);
        uninit(ao);
        return -1;
    }
    pthread_mutex_unlock(&p->lock);
    if (!feed_pending_packets(ao)) {
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
        pthread_mutex_lock(&p->lock);
        free_pending_packets_locked(p);
        pthread_mutex_unlock(&p->lock);
    }
    if (p->lock_initialized) {
        pthread_mutex_destroy(&p->lock);
        p->lock_initialized = false;
    }
}

static void reset(struct ao *ao)
{
    struct priv *p = ao->priv;
    int64_t reset_target_ns = 0;
    bool needs_segment_prime = false;

    pthread_mutex_lock(&p->lock);
    p->paused = false;
    p->playing = false;
    p->primed = false;
    p->logged_write = false;
    p->buffered_samples = 0;
    p->written_samples = 0;
    free_pending_packets_locked(p);
    if (p->fifo)
        av_audio_fifo_drain(p->fifo, av_audio_fifo_size(p->fifo));
    if (p->encoder && !reopen_encoder_locked(ao))
        MP_WARN(ao, "Unable to reopen Starfish AAC encoder on reset\n");
    if (p->ctx && starfish_ctx_get_audio_reset_target_ns(p->ctx, &reset_target_ns,
                                                         &needs_segment_prime)) {
        if (needs_segment_prime) {
            p->needs_sync = true;
            MP_INFO(ao, "ao_starfish reset waiting for segment prime target=%.3f\n",
                    (double)reset_target_ns / 1000000000.0);
        } else if (!prime_at_ns_locked(ao, reset_target_ns, "audio reset")) {
            p->needs_sync = true;
            MP_WARN(ao, "Unable to prime Starfish audio reset at %.3f\n",
                    (double)reset_target_ns / 1000000000.0);
        }
    } else {
        p->needs_sync = true;
    }
    pthread_mutex_unlock(&p->lock);
    if (p->ctx)
        ao_wakeup(ao);
}

static void start(struct ao *ao)
{
    struct priv *p = ao->priv;
    bool prime_ok = true;

    pthread_mutex_lock(&p->lock);
    p->paused = false;
    p->playing = true;
    p->last_time = mp_time_sec();
    MP_INFO(ao, "ao_starfish start\n");
    if (p->needs_sync) {
        MP_INFO(ao, "ao_starfish start waiting for Starfish segment audio prime\n");
    }
    if (!p->needs_sync && !ensure_audio_primed(ao))
        prime_ok = false;
    pthread_mutex_unlock(&p->lock);
    if (!prime_ok || !feed_pending_packets(ao))
        MP_WARN(ao, "Unable to re-prime Starfish audio on start\n");
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
        feed_pending_packets(ao);
        pthread_mutex_lock(&p->lock);
        p->last_time = mp_time_sec();
        pthread_mutex_unlock(&p->lock);
        return starfish_ctx_resume(p->ctx);
    }
    return true;
}

static bool audio_write(struct ao *ao, void **data, int samples)
{
    struct priv *p = ao->priv;
    bool ok = false;

    if (!feed_pending_packets(ao))
        return false;

    pthread_mutex_lock(&p->lock);
    if (p->needs_sync) {
        MP_TRACE(ao, "ao_starfish sync: waiting for segment audio prime\n");
        goto done;
    }
    if (!p->logged_write) {
        MP_INFO(ao, "ao_starfish first write samples=%d\n", samples);
        p->logged_write = true;
    }
    if (!ensure_audio_primed(ao))
        goto done;
    if (p->pcm_mode) {
        // data[0] is interleaved S16 (single plane); queue it verbatim as a
        // PCM ES frame with a sample-accurate PTS.
        int64_t pts_ns = av_rescale_q(p->written_samples, audio_time_base(ao),
                                      (AVRational){1, 1000000000});
        if (pts_ns < 0)
            pts_ns = 0;
        const size_t bytes = (size_t)samples * p->bytes_per_frame;
        if (!queue_encoded_packet_locked(ao, data[0], bytes,
                                         apply_audio_delay_to_pts(ao, pts_ns),
                                         samples))
            goto done;
        p->written_samples += samples;
        if (p->buffered_samples < p->latency_samples)
            p->buffered_samples = p->latency_samples;
        ok = true;
        goto done;
    }
    if (av_audio_fifo_realloc(p->fifo, av_audio_fifo_size(p->fifo) + samples) < 0)
        goto done;
    if (av_audio_fifo_write(p->fifo, data, samples) < samples)
        goto done;
    if (!encode_pending_audio(ao, false))
        goto done;
    if (p->buffered_samples < p->latency_samples)
        p->buffered_samples = p->latency_samples;
    ok = true;

done:
    pthread_mutex_unlock(&p->lock);
    if (ok && !feed_pending_packets(ao))
        ok = false;
    return ok;
}

static void get_state(struct ao *ao, struct mp_pcm_state *state)
{
    struct priv *p = ao->priv;
    int queued_fifo = 0;
    double queued_total = 0;

    feed_pending_packets(ao);

    pthread_mutex_lock(&p->lock);
    queued_fifo = p->fifo ? av_audio_fifo_size(p->fifo) : 0;
    drain(ao);
    queued_total = p->buffered_samples + p->pending_samples + queued_fifo;

    state->queued_samples = queued_total;
    state->free_samples = MPMAX(ao->device_buffer - p->latency_samples -
                                state->queued_samples, 0);
    state->free_samples = state->free_samples / p->outburst * p->outburst;
    if (p->feed_blocked)
        state->free_samples = 0;
    state->delay = queued_total / ao->samplerate;
    state->playing = p->playing && !p->paused && !p->needs_sync &&
                     queued_total > 0;
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
