/*
 * This file is part of mpv.
 */

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <libavutil/mathematics.h>
#include <libavutil/mem.h>

#include "audio/aframe.h"
#include "audio/chmap.h"
#include "audio/format.h"
#include "audio/out/ao.h"
#include "audio/out/internal.h"
#include "common/common.h"
#include "common/msg.h"
#include "options/m_config_core.h"
#include "options/m_option.h"
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
    int frame_samples;
    int latency_samples;
    int outburst;
    int64_t written_samples;
    int64_t last_written_pts_ns;
    int64_t preroll_anchor_pts_ns;
    bool primed;
    bool logged_write;
    bool needs_sync;
    bool logged_audio_delay;
    double last_audio_delay;
    bool audio_delay_initialized;
    double requested_audio_delay;
    double active_audio_delay;
    bool audio_delay_pending;
    struct encoded_packet *pending_head;
    struct encoded_packet *pending_tail;
    int pending_samples;
    bool feed_blocked;
    /* Single-flight guard for feed_pending_packets: the drain loop is entered
     * from the AO thread (write/get_state/start/set_pause) and from the ctx
     * worker thread (audio_prime_cb). Two concurrent drains would interleave
     * pop->feed sequences and hand Starfish audio ES packets out of PTS
     * order. */
    bool feeding;
    /* Bumped by free_pending_packets_locked. A packet popped for feeding
     * before a flush must not be re-queued (or accounted) after it. */
    uint64_t pending_generation;
    /* Size of one interleaved PCM sample-frame (channels * bytes-per-sample). */
    int bytes_per_frame;
    double feed_ahead;
};

#define STARFISH_PCM_TARGET_LATENCY_SEC 0.04
// Conduit buffer between mpv and Starfish only. A large value here makes mpv
// wait too long before it declares audio ready, leaving video preroll ahead of
// the first real audio packets. Keep the mpv side close to the actual target
// latency and let the configurable Starfish feed window own play-ahead.
#define STARFISH_PCM_BUFFER_SEC 0.20
#define STARFISH_PCM_START_PRIME_FRAMES 1
#define STARFISH_DEFAULT_FEED_AHEAD_SEC 0.40

// Decoded audio is fed to Starfish as a raw-PCM elementary stream.
#define STARFISH_PCM_FORMAT AF_FORMAT_S16
#define STARFISH_PCM_BITS_PER_SAMPLE 16
#define STARFISH_PCM_FORMAT_TOKEN "S16LE"
#define STARFISH_PCM_LAYOUT_TOKEN "interleaved"
// PCM output rate fed to mpv's filter chain, used as the audio time-base for
// PTS, and reported in the load payload (libpf maps it to its sampleRate
// enum). 48 kHz is the only rate worth targeting today: libpf's
// LUT_SampleRateType has no 44.1 kHz entry, so 44.1 only ever shows up as a
// downstream fallback when the enum lookup fails.
#define STARFISH_PCM_OUTPUT_RATE 48000

static void uninit(struct ao *ao);
static bool prime_at_ns_locked(struct ao *ao, int64_t pts_ns, const char *reason);
static bool prime_pcm_silence(struct ao *ao, int frames, const char *reason);
static bool audio_prime_cb(void *opaque, int64_t pts_ns);

static inline AVRational audio_time_base(struct ao *ao)
{
    return (AVRational){1, ao->samplerate};
}

static int64_t audio_pts_to_samples(struct ao *ao, double pts)
{
    if (pts == MP_NOPTS_VALUE)
        return INT64_MIN;
    int64_t samples = llround(pts * ao->samplerate);
    return MPMAX(samples, 0);
}

static double current_audio_delay(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (!p->opts_cache || !p->opts)
        return 0.0;

    m_config_cache_update(p->opts_cache);
    return p->opts->audio_delay;
}

static double queued_audio_samples_locked(struct ao *ao)
{
    struct priv *p = ao->priv;
    struct starfish_audio_status status = {0};
    if (p->last_written_pts_ns != INT64_MIN) {
        if (p->ctx)
            starfish_ctx_get_audio_status(p->ctx, &status);
        int64_t anchor = status.clock_valid ? status.clock_pts_ns
                                           : p->preroll_anchor_pts_ns;
        double ahead = anchor == INT64_MIN
            ? 0 : (p->last_written_pts_ns - anchor) / 1e9;
        return MPMAX(ahead * ao->samplerate, 0);
    }
    return p->pending_samples;
}

static void latch_audio_delay_locked(struct ao *ao, double delay,
                                     const char *reason)
{
    struct priv *p = ao->priv;
    p->requested_audio_delay = delay;
    p->active_audio_delay = delay;
    p->audio_delay_pending = false;
    p->audio_delay_initialized = true;
    MP_INFO(ao, "ao_starfish audio-delay effective %.3f (%s)\n",
            p->active_audio_delay, reason ? reason : "latch");
}

static double active_audio_delay_locked(struct ao *ao)
{
    struct priv *p = ao->priv;
    double requested = current_audio_delay(ao);

    if (!p->audio_delay_initialized)
        latch_audio_delay_locked(ao, requested, "init");

    if (fabs(requested - p->requested_audio_delay) >= 0.0005) {
        double queued = queued_audio_samples_locked(ao);
        p->requested_audio_delay = requested;
        if (queued <= p->frame_samples) {
            latch_audio_delay_locked(ao, requested, "no queued audio");
        } else {
            p->audio_delay_pending = true;
            MP_INFO(ao,
                    "ao_starfish audio-delay requested %.3f; waiting for "
                    "%.3fs queued audio before latch\n",
                    requested, queued / ao->samplerate);
        }
    }

    if (p->audio_delay_pending &&
        queued_audio_samples_locked(ao) <= p->frame_samples)
        latch_audio_delay_locked(ao, p->requested_audio_delay, "queue drained");

    return p->active_audio_delay;
}

// Starfish does A/V sync internally from the PTS we attach to each audio ES
// packet, so accurate PTS is what keeps lipsync; there is no AO-side delay
// compensation needed. We add the user's audio-delay opt here so the setting
// in the UI (used to dial in TV/AVR processing latency) actually shifts where
// audio lands relative to video without touching the video PTS path.
static int64_t apply_audio_delay_to_pts_locked(struct ao *ao, int64_t pts_ns)
{
    struct priv *p = ao->priv;
    const double delay = active_audio_delay_locked(ao);
    const int64_t delayed_pts_ns =
        pts_ns + (int64_t)llround(delay * 1000000000.0);

    if (!p->logged_audio_delay || fabs(delay - p->last_audio_delay) >= 0.0005) {
        MP_INFO(ao, "ao_starfish audio-delay applied delay=%.3f base_pts=%.3f feed_pts=%.3f\n",
                delay,
                (double)pts_ns / 1000000000.0,
                (double)MPMAX(delayed_pts_ns, 0) / 1000000000.0);
        p->logged_audio_delay = true;
        p->last_audio_delay = delay;
    }

    return MPMAX(delayed_pts_ns, 0);
}

static void wake_ao(void *opaque)
{
    ao_wakeup(opaque);
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
    p->pending_generation++;
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
    if (p->preroll_anchor_pts_ns == INT64_MIN)
        p->preroll_anchor_pts_ns = pts_ns;
    p->last_written_pts_ns = pts_ns +
        av_rescale_q(samples, audio_time_base(ao),
                     (AVRational){1, 1000000000});
    p->feed_blocked = false;
    return true;
}

static bool feed_pending_packets(struct ao *ao)
{
    struct priv *p = ao->priv;
    bool ok = true;

    pthread_mutex_lock(&p->lock);
    if (p->feeding) {
        // Another thread is mid-drain; a second drain would reorder packets.
        // The active drain will pick up anything we queued.
        pthread_mutex_unlock(&p->lock);
        return true;
    }
    p->feeding = true;
    pthread_mutex_unlock(&p->lock);

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
        const uint64_t gen = p->pending_generation;
        struct starfish_ctx *ctx = starfish_ctx_retain(p->ctx);
        pthread_mutex_unlock(&p->lock);

        int r = starfish_ctx_feed_audio(ctx, pkt->data, pkt->size, pkt->pts_ns);
        starfish_ctx_unref(ctx);
        if (r == STARFISH_FEED_AGAIN) {
            pthread_mutex_lock(&p->lock);
            bool flushed = p->pending_generation != gen;
            if (!flushed) {
                pkt->next = p->pending_head;
                p->pending_head = pkt;
                if (!p->pending_tail)
                    p->pending_tail = pkt;
                p->pending_samples += pkt->samples;
                p->feed_blocked = true;
            }
            pthread_mutex_unlock(&p->lock);
            if (flushed)
                free_encoded_packet(pkt);
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
        pthread_mutex_unlock(&p->lock);
        free_encoded_packet(pkt);

        if (!ok)
            break;
    }

    pthread_mutex_lock(&p->lock);
    p->feeding = false;
    pthread_mutex_unlock(&p->lock);
    return ok;
}

static bool sync_written_samples_to_seek_target(struct ao *ao, bool log_missing)
{
    struct priv *p = ao->priv;
    int64_t seek_target_ns = 0;

    if (!p->ctx)
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

// Queue zeroed PCM so the pipeline has a little audio ahead of the first real
// samples. Lock held.
static bool prime_pcm_silence(struct ao *ao, int frames, const char *reason)
{
    struct priv *p = ao->priv;

    if (frames <= 0)
        return true;

    const size_t max_bytes = (size_t)p->frame_samples * p->bytes_per_frame;
    uint8_t *silence = av_mallocz(max_bytes);
    if (!silence) {
        MP_WARN(ao, "Failed to allocate PCM silence for %s\n",
                reason ? reason : "prime");
        return false;
    }

    const int total_samples = frames * p->frame_samples;
    const int64_t real_start_samples = p->written_samples;
    int64_t silence_samples = real_start_samples - total_samples;
    if (silence_samples < 0)
        silence_samples = 0;

    bool ok = true;
    int queued_samples = 0;
    while (silence_samples < real_start_samples) {
        int chunk = (int)MPMIN((int64_t)p->frame_samples,
                               real_start_samples - silence_samples);
        int64_t pts_ns = av_rescale_q(silence_samples, audio_time_base(ao),
                                      (AVRational){1, 1000000000});
        if (pts_ns < 0)
            pts_ns = 0;
        if (!queue_encoded_packet_locked(ao, silence,
                                         (size_t)chunk * p->bytes_per_frame,
                                         apply_audio_delay_to_pts_locked(ao, pts_ns),
                                         chunk)) {
            ok = false;
            break;
        }
        silence_samples += chunk;
        queued_samples += chunk;
    }
    p->written_samples = real_start_samples;
    av_free(silence);
    if (ok) {
        MP_INFO(ao, "%s Starfish audio with %d silent PCM pre-roll samples\n",
                reason ? reason : "Primed", queued_samples);
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
    if (!prime_pcm_silence(ao, STARFISH_PCM_START_PRIME_FRAMES,
                           "Pre-primed")) {
        return false;
    }

    p->primed = true;
    return true;
}

static bool prime_at_ns_locked(struct ao *ao, int64_t pts_ns, const char *reason)
{
    struct priv *p = ao->priv;

    if (pts_ns < 0)
        return false;

    int64_t target_samples = av_rescale_q(pts_ns, (AVRational){1, 1000000000},
                                          audio_time_base(ao));
    // "Keep existing prime" only handles the startup race where mpv pre-feeds
    // a few hundred ms of audio before Starfish issues its first segment-
    // prime callback (target lands just behind what we already wrote). A real
    // backward seek lands much further back and must flush + re-prime, or
    // audio will stay positioned at the pre-seek PTS while video jumps.
    const int64_t keep_window =
        (int64_t)(STARFISH_PCM_BUFFER_SEC * ao->samplerate) + p->latency_samples;
    if (p->primed && target_samples <= p->written_samples &&
        p->written_samples - target_samples <= keep_window) {
        p->needs_sync = false;
        MP_INFO(ao,
                "ao_starfish keep existing PCM prime at %s pts=%" PRId64
                " target_samples=%" PRId64 " written_samples=%" PRId64 "\n",
                reason ? reason : "request", pts_ns, target_samples,
                p->written_samples);
        return true;
    }

    free_pending_packets_locked(p);
    p->written_samples = target_samples;
    p->last_written_pts_ns = INT64_MIN;
    p->preroll_anchor_pts_ns = INT64_MIN;
    p->primed = false;
    p->needs_sync = false;
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
    p->last_written_pts_ns = INT64_MIN;
    p->preroll_anchor_pts_ns = INT64_MIN;

    // Starfish PCM is interleaved S16 at 48 kHz. Non-interleaved buffers
    // would require explicit plane packing that mpv does not provide here.
    ao->samplerate = STARFISH_PCM_OUTPUT_RATE;
    ao->channels = (struct mp_chmap)MP_CHMAP_INIT_STEREO;
    ao->format = STARFISH_PCM_FORMAT;
    p->bytes_per_frame = ao->channels.num * (STARFISH_PCM_BITS_PER_SAMPLE / 8);
    p->frame_samples = 1024;
    p->outburst = p->frame_samples;
    p->latency_samples = ao->samplerate * STARFISH_PCM_TARGET_LATENCY_SEC;
    if (!starfish_ctx_set_audio_feed_ahead(p->ctx, p->feed_ahead)) {
        MP_VERBOSE(ao, "Failed to configure Starfish audio feed-ahead\n");
        uninit(ao);
        return -1;
    }

    if (!starfish_ctx_configure_audio_pcm(p->ctx, ao->channels.num,
                                          ao->samplerate,
                                          STARFISH_PCM_BITS_PER_SAMPLE,
                                          STARFISH_PCM_FORMAT_TOKEN,
                                          STARFISH_PCM_LAYOUT_TOKEN)) {
        MP_VERBOSE(ao, "Failed to configure Starfish PCM audio\n");
        uninit(ao);
        return -1;
    }

    MP_INFO(ao, "ao_starfish init PCM samplerate=%d channels=%d frame_samples=%d\n",
            ao->samplerate, ao->channels.num, p->frame_samples);
    starfish_ctx_set_wakeup_cb(p->ctx, STARFISH_STREAM_AUDIO, wake_ao, ao);
    starfish_ctx_set_audio_prime_cb(p->ctx, audio_prime_cb, ao);
    pthread_mutex_lock(&p->lock);
    active_audio_delay_locked(ao);
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
    ao->device_buffer = ao->samplerate * p->feed_ahead;
    MP_INFO(ao, "ao_starfish buffering latency=%d device_buffer=%d\n",
            p->latency_samples, ao->device_buffer);
    return 0;
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;

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
    p->primed = false;
    p->logged_write = false;
    p->audio_delay_pending = false;
    p->written_samples = 0;
    p->last_written_pts_ns = INT64_MIN;
    p->preroll_anchor_pts_ns = INT64_MIN;
    free_pending_packets_locked(p);
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

    if (p->ctx) {
        if (paused)
            return starfish_ctx_pause(p->ctx);
        feed_pending_packets(ao);
        return starfish_ctx_resume(p->ctx);
    }
    return true;
}

static bool audio_write(struct ao *ao, void **data, int samples)
{
    struct priv *p = ao->priv;
    bool ok = false;
    struct mp_aframe *af = *(struct mp_aframe **)data;
    if (!af)
        return false;

    void **planes = (void **)mp_aframe_get_data_rw(af);
    if (!planes)
        return false;
    samples = mp_aframe_get_size(af);
    if (samples <= 0)
        return true;
    double frame_pts = mp_aframe_get_pts(af);

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
    // planes[0] is interleaved S16. Frame-sized access units preserve the PTS
    // cadence expected by the Starfish PCM pipeline.
    int64_t frame_samples = audio_pts_to_samples(ao, frame_pts);
    if (frame_samples != INT64_MIN) {
        int64_t delta = frame_samples - p->written_samples;
        if (llabs(delta) > p->frame_samples) {
            MP_VERBOSE(ao,
                       "ao_starfish PCM PTS realign frame_pts=%.6f "
                       "sample_delta=%" PRId64 "\\n",
                       frame_pts, delta);
        }
        p->written_samples = frame_samples;
    }

    uint8_t *src = planes[0];
    for (int pos = 0; pos < samples; ) {
        int chunk = MPMIN(p->frame_samples, samples - pos);
        int64_t pts_ns = av_rescale_q(p->written_samples,
                                      audio_time_base(ao),
                                      (AVRational){1, 1000000000});
        if (pts_ns < 0)
            pts_ns = 0;
        const size_t bytes = (size_t)chunk * p->bytes_per_frame;
        if (!queue_encoded_packet_locked(ao,
                                         src + (size_t)pos * p->bytes_per_frame,
                                         bytes,
                                         apply_audio_delay_to_pts_locked(ao, pts_ns),
                                         chunk))
            goto done;
        p->written_samples += chunk;
        pos += chunk;
    }
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
    struct starfish_audio_status status = {0};

    feed_pending_packets(ao);
    if (p->ctx)
        starfish_ctx_get_audio_status(p->ctx, &status);

    pthread_mutex_lock(&p->lock);
    active_audio_delay_locked(ao);

    int64_t anchor = status.clock_valid ? status.clock_pts_ns
                                       : p->preroll_anchor_pts_ns;
    double ahead = 0;
    if (p->last_written_pts_ns != INT64_MIN && anchor != INT64_MIN)
        ahead = MPMAX((p->last_written_pts_ns - anchor) / 1e9, 0);

    state->delay = status.clock_valid ? ahead : 0;
    state->queued_samples = MPCLAMP(llround(ahead * ao->samplerate), 0,
                                    ao->device_buffer);
    int target_samples = ao->samplerate * p->feed_ahead;
    state->free_samples = MPMAX(target_samples -
                                llround(ahead * ao->samplerate), 0);
    state->free_samples = state->free_samples / p->outburst * p->outburst;
    if (p->needs_sync || p->feed_blocked)
        state->free_samples = 0;
    if (p->audio_delay_pending)
        state->free_samples = 0;
    state->playing = status.playing && status.fed;
    pthread_mutex_unlock(&p->lock);
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_starfish = {
    .description = "LG webOS Starfish",
    .name = "starfish",
    .write_frames = true,
    .init = init,
    .uninit = uninit,
    .reset = reset,
    .get_state = get_state,
    .set_pause = set_pause,
    .write = audio_write,
    .start = start,
    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv) {
        .feed_ahead = STARFISH_DEFAULT_FEED_AHEAD_SEC,
    },
    .options = (const struct m_option[]) {
        {"feed-ahead", OPT_DOUBLE(feed_ahead), M_RANGE(0.05, 2.0)},
        {0}
    },
    .options_prefix = "ao-starfish",
};
