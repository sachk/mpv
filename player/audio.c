/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "mpv_talloc.h"

#include "common/common.h"
#include "common/encode.h"
#include "common/msg.h"
#include "options/options.h"
#include "osdep/timer.h"

#include "audio/format.h"
#include "audio/out/ao.h"
#include "demux/demux.h"
#include "filters/f_async_queue.h"
#include "filters/f_decoder_wrapper.h"
#include "filters/filter_internal.h"
#include "video/out/vo.h"

#include "command.h"
#include "core.h"
#include "starfish_sync.h"

enum {
    AD_OK = 0,
    AD_EOF = -2,
    AD_WAIT = -4,
};

#define STARFISH_AUDIO_SYNC_PERIOD_NS (50LL * 1000 * 1000)
// Use a slow PI controller for the independent ALSA and Starfish clocks. The
// integral term learns their persistent rate difference, while the phase term
// gently pulls short-lived error back toward the deadband. Speed changes are
// slew-limited so a single noisy clock sample cannot cause an audible jump.
#define STARFISH_AUDIO_SYNC_FILTER_TIME 1.00
#define STARFISH_AUDIO_SYNC_PHASE_RECOVERY_TIME 2.00
#define STARFISH_AUDIO_SYNC_RATE_LEARN_TIME 8.00
#define STARFISH_AUDIO_SYNC_LOG_PERIOD_NS (1000LL * 1000 * 1000)
#define STARFISH_AUDIO_SYNC_SETTLE_NS (500LL * 1000 * 1000)
#define STARFISH_AUDIO_SYNC_DEADBAND 0.010
#define STARFISH_AUDIO_SYNC_RATE_BIAS_MAX 0.005
#define STARFISH_AUDIO_SYNC_CORRECTION_MAX 0.010
#define STARFISH_AUDIO_SYNC_SLEW_PER_SEC 0.003
#define STARFISH_AUDIO_SYNC_HARD_REALIGN_THRESHOLD 0.150
#define STARFISH_AUDIO_SYNC_HARD_REALIGN_CONFIRM_NS (500LL * 1000 * 1000)
#define STARFISH_AUDIO_SYNC_HARD_REALIGN_COOLDOWN_NS (2000LL * 1000 * 1000)
#define STARFISH_AUDIO_START_REALIGN_THRESHOLD 0.080

static void ao_process(struct mp_filter *f);
static void reset_starfish_audio_sync(struct MPContext *mpctx);
static void realign_starfish_audio_start(struct MPContext *mpctx, double pts);

static void update_speed_filters(struct MPContext *mpctx)
{
    struct ao_chain *ao_c = mpctx->ao_chain;
    if (!ao_c)
        return;

    double pitch = mpctx->opts->playback_pitch;
    double speed = mpctx->opts->playback_speed;
    double resample = mpctx->speed_factor_a;
    double drop = 1.0;

    if (!mpctx->opts->pitch_correction) {
        resample *= speed;
        speed = 1.0;
    }

    int video_sync = mpctx->display_sync_active ? mpctx->video_out->opts->video_sync : VS_NONE;
    switch (video_sync) {
    case VS_DISP_ADROP:
        drop *= speed * resample / pitch;
        resample = pitch;
        speed = 1.0;
        break;
    case VS_DISP_TEMPO:
        speed = mpctx->audio_speed / pitch;
        resample = pitch;
        break;
    default:
        resample *= pitch;
        speed /= pitch;
        break;
    }

    mp_output_chain_set_audio_speed(ao_c->filter, speed, resample, drop);
}

static int recreate_audio_filters(struct MPContext *mpctx)
{
    struct ao_chain *ao_c = mpctx->ao_chain;
    mp_assert(ao_c);

    if (!mp_output_chain_update_filters(ao_c->filter, mpctx->opts->af_settings))
        goto fail;

    update_speed_filters(mpctx);

    mp_notify(mpctx, MPV_EVENT_AUDIO_RECONFIG, NULL);

    return 0;

fail:
    MP_ERR(mpctx, "Audio filter initialized failed!\n");
    return -1;
}

int reinit_audio_filters(struct MPContext *mpctx)
{
    struct ao_chain *ao_c = mpctx->ao_chain;
    if (!ao_c)
        return 0;

    double delay = mp_output_get_measured_total_delay(ao_c->filter);

    if (recreate_audio_filters(mpctx) < 0)
        return -1;

    double ndelay = mp_output_get_measured_total_delay(ao_c->filter);

    // Only force refresh if the amount of dropped buffered data is going to
    // cause "issues" for the A/V sync logic.
    if (mpctx->audio_status == STATUS_PLAYING && delay - ndelay >= 0.2)
        issue_refresh_seek(mpctx, MPSEEK_EXACT);
    return 1;
}

static double db_gain(double db)
{
    return pow(10.0, db / 20.0);
}

static float compute_replaygain(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;

    float rgain = 1.0;

    struct replaygain_data *rg = NULL;
    struct track *track = mpctx->current_track[0][STREAM_AUDIO];
    if (track)
        rg = track->stream->codec->replaygain_data;
    if (opts->rgain_mode && rg) {
        MP_VERBOSE(mpctx, "Replaygain: Track=%f/%f Album=%f/%f\n", rg->track_gain, rg->track_peak, rg->album_gain,
            rg->album_peak);

        float gain, peak;
        if (opts->rgain_mode == 1) {
            gain = rg->track_gain;
            peak = rg->track_peak;
        } else {
            gain = rg->album_gain;
            peak = rg->album_peak;
        }

        gain += opts->rgain_preamp;
        rgain = db_gain(gain);

        MP_VERBOSE(mpctx, "Applying replay-gain: %f\n", rgain);

        if (!opts->rgain_clip) { // clipping prevention
            rgain = MPMIN(rgain, 1.0 / peak);
            MP_VERBOSE(mpctx, "...with clipping prevention: %f\n", rgain);
        }
    } else if (opts->rgain_fallback) {
        rgain = db_gain(opts->rgain_fallback);
        MP_VERBOSE(mpctx, "Applying fallback gain: %f\n", rgain);
    }

    return rgain;
}

float audio_get_gain(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    float gain = MPMAX(opts->softvol_volume / 100.0, 0);
    gain = pow(gain, 3);
    gain *= compute_replaygain(mpctx);
    gain *= db_gain(opts->softvol_gain);
    if (opts->softvol_mute)
        gain = 0.0;
    return gain;
}

// Called when opts->softvol_volume or opts->softvol_mute were changed.
void audio_update_volume(struct MPContext *mpctx)
{
    struct ao_chain *ao_c = mpctx->ao_chain;
    if (!ao_c || !ao_c->ao)
        return;

    float gain = audio_get_gain(mpctx);
    ao_set_gain(ao_c->ao, gain);
}

// Call this if opts->playback_speed or mpctx->speed_factor_* change.
void update_playback_speed(struct MPContext *mpctx)
{
    mpctx->audio_speed = mpctx->opts->playback_speed * mpctx->speed_factor_a;
    mpctx->video_speed = mpctx->opts->playback_speed * mpctx->speed_factor_v;

    if (is_starfish_video_out(mpctx))
        vo_control(mpctx->video_out, VOCTRL_SET_PLAYBACK_SPEED, &mpctx->opts->playback_speed);

    update_speed_filters(mpctx);
}

static bool has_video_track(struct MPContext *mpctx)
{
    if (mpctx->vo_chain && mpctx->vo_chain->is_coverart)
        return false;

    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *track = mpctx->tracks[n];
        if (track->type == STREAM_VIDEO && !track->attached_picture && !track->image)
            return true;
    }

    return false;
}

static void ao_chain_reset_state(struct ao_chain *ao_c)
{
    ao_c->last_out_pts = MP_NOPTS_VALUE;
    ao_c->out_eof = false;
    ao_c->start_pts_known = false;
    ao_c->start_pts = MP_NOPTS_VALUE;
    ao_c->untimed_throttle = false;
    ao_c->underrun = false;
    ao_c->delaying_audio_start = false;
}

void reset_audio_state(struct MPContext *mpctx)
{
    if (mpctx->ao_chain) {
        ao_chain_reset_state(mpctx->ao_chain);
        struct track *t = mpctx->ao_chain->track;
        if (t && t->dec)
            mp_decoder_wrapper_set_play_dir(t->dec, mpctx->play_dir);
    }
    mpctx->audio_status = mpctx->ao_chain ? STATUS_SYNCING : STATUS_EOF;
    mpctx->delay = 0;
    mpctx->logged_async_diff = -1;
    mpctx->starfish_audio_sync_last_realign_ns = 0;
    mpctx->starfish_audio_clock_wait_logged = false;
    mpctx->starfish_audio_sync_resync_pending = false;
    mpctx->starfish_audio_start_realign_done = false;
    reset_starfish_audio_sync(mpctx);
}

void uninit_audio_out(struct MPContext *mpctx)
{
    struct ao_chain *ao_c = mpctx->ao_chain;
    if (ao_c) {
        ao_c->ao_queue = NULL;
        TA_FREEP(&ao_c->queue_filter);
        ao_c->ao = NULL;
    }
    if (mpctx->ao) {
        // Note: with gapless_audio, stop_play is not correctly set
        if ((mpctx->opts->gapless_audio || mpctx->stop_play == AT_END_OF_FILE) && ao_is_playing(mpctx->ao)
            && !get_internal_paused(mpctx)) {
            MP_VERBOSE(mpctx, "draining left over audio\n");
            ao_drain(mpctx->ao);
        }
        ao_uninit(mpctx->ao);

        mp_notify(mpctx, MPV_EVENT_AUDIO_RECONFIG, NULL);
    }
    mpctx->ao = NULL;
    TA_FREEP(&mpctx->ao_filter_fmt);
}

static void ao_chain_uninit(struct ao_chain *ao_c)
{
    struct track *track = ao_c->track;
    if (track) {
        mp_assert(track->ao_c == ao_c);
        track->ao_c = NULL;
        if (ao_c->dec_src)
            mp_assert(track->dec->f->pins[0] == ao_c->dec_src);
        talloc_free(track->dec->f);
        track->dec = NULL;
    }

    if (ao_c->filter_src)
        mp_pin_disconnect(ao_c->filter_src);

    talloc_free(ao_c->filter->f);
    talloc_free(ao_c->ao_filter);
    talloc_free(ao_c);
}

void uninit_audio_chain(struct MPContext *mpctx)
{
    if (mpctx->ao_chain) {
        ao_chain_uninit(mpctx->ao_chain);
        mpctx->ao_chain = NULL;

        mpctx->audio_status = STATUS_EOF;

        mp_notify(mpctx, MPV_EVENT_AUDIO_RECONFIG, NULL);
    }
}

static char *audio_config_to_str_buf(char *buf, size_t buf_sz, int rate, int format, struct mp_chmap channels)
{
    char ch[128];
    mp_chmap_to_str_buf(ch, sizeof(ch), &channels);
    char *hr_ch = mp_chmap_to_str_hr(&channels);
    if (strcmp(hr_ch, ch) != 0)
        mp_snprintf_cat(ch, sizeof(ch), " (%s)", hr_ch);
    snprintf(buf, buf_sz, "%dHz %s %dch %s", rate, ch, channels.num, af_fmt_to_str(format));
    return buf;
}

// Decide whether on a format change, we should reinit the AO.
static bool keep_weak_gapless_format(struct mp_aframe *old, struct mp_aframe *new)
{
    bool res = false;
    struct mp_aframe *new_mod = mp_aframe_new_ref(new);
    MP_HANDLE_OOM(new_mod);

    // If the sample formats are compatible (== libswresample generally can
    // convert them), keep the AO. On other changes, recreate it.

    int old_fmt = mp_aframe_get_format(old);
    int new_fmt = mp_aframe_get_format(new);

    if (af_format_conversion_score(old_fmt, new_fmt) == INT_MIN)
        goto done; // completely incompatible formats

    if (!mp_aframe_set_format(new_mod, old_fmt))
        goto done;

    res = mp_aframe_config_equals(old, new_mod);

done:
    talloc_free(new_mod);
    return res;
}

static void ao_chain_set_ao(struct ao_chain *ao_c, struct ao *ao)
{
    if (ao_c->ao != ao) {
        mp_assert(!ao_c->ao);
        ao_c->ao = ao;
        ao_c->ao_queue = ao_get_queue(ao_c->ao);
        ao_c->queue_filter = mp_async_queue_create_filter(ao_c->ao_filter, MP_PIN_IN, ao_c->ao_queue);
        mp_async_queue_set_notifier(ao_c->queue_filter, ao_c->ao_filter);
        // Make sure filtering never stops with frames stuck in access filter.
        mp_filter_set_high_priority(ao_c->queue_filter, true);
        audio_update_volume(ao_c->mpctx);
    }

    if (ao_c->filter->ao_needs_update)
        mp_output_chain_set_ao(ao_c->filter, ao_c->ao);

    mp_filter_wakeup(ao_c->ao_filter);
}

static int reinit_audio_filters_and_output(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;
    struct ao_chain *ao_c = mpctx->ao_chain;
    mp_assert(ao_c);
    struct track *track = ao_c->track;

    mp_assert(ao_c->filter->ao_needs_update);

    // The "ideal" filter output format
    struct mp_aframe *out_fmt = mp_aframe_new_ref(ao_c->filter->output_aformat);
    MP_HANDLE_OOM(out_fmt);

    if (!mp_aframe_config_is_valid(out_fmt)) {
        talloc_free(out_fmt);
        goto init_error;
    }

    if (af_fmt_is_pcm(mp_aframe_get_format(out_fmt))) {
        if (opts->force_srate)
            mp_aframe_set_rate(out_fmt, opts->force_srate);
        if (opts->audio_output_format)
            mp_aframe_set_format(out_fmt, opts->audio_output_format);
        if (opts->audio_output_channels.num_chmaps == 1)
            mp_aframe_set_chmap(out_fmt, &opts->audio_output_channels.chmaps[0]);
    }

    // Weak gapless audio: if the filter output format is the same as the
    // previous one, keep the AO and don't reinit anything.
    // Strong gapless: always keep the AO
    if ((mpctx->ao_filter_fmt && mpctx->ao && opts->gapless_audio < 0
            && keep_weak_gapless_format(mpctx->ao_filter_fmt, out_fmt))
        || (mpctx->ao && opts->gapless_audio > 0)) {
        ao_chain_set_ao(ao_c, mpctx->ao);
        talloc_free(out_fmt);
        return 0;
    }

    // Format change during syncing. Force playback start early, then wait.
    if (ao_c->ao_queue && mp_async_queue_get_frames(ao_c->ao_queue) && mpctx->audio_status == STATUS_SYNCING) {
        mpctx->audio_status = STATUS_READY;
        mp_wakeup_core(mpctx);
        talloc_free(out_fmt);
        return 0;
    }
    if (mpctx->audio_status == STATUS_READY) {
        talloc_free(out_fmt);
        return 0;
    }

    uninit_audio_out(mpctx);

    int out_rate = mp_aframe_get_rate(out_fmt);
    int out_format = mp_aframe_get_format(out_fmt);
    struct mp_chmap out_channels = { 0 };
    mp_aframe_get_chmap(out_fmt, &out_channels);

    int ao_flags = 0;
    bool spdif_fallback = af_fmt_is_spdif(out_format) && ao_c->spdif_passthrough;

    if (opts->ao_null_fallback && !spdif_fallback)
        ao_flags |= AO_INIT_NULL_FALLBACK;

    if (opts->audio_stream_silence)
        ao_flags |= AO_INIT_STREAM_SILENCE;

    if (opts->audio_exclusive)
        ao_flags |= AO_INIT_EXCLUSIVE;

    if (af_fmt_is_pcm(out_format)) {
        if (!opts->audio_output_channels.set || opts->audio_output_channels.auto_safe)
            ao_flags |= AO_INIT_SAFE_MULTICHANNEL_ONLY;

        mp_chmap_sel_list(&out_channels, opts->audio_output_channels.chmaps, opts->audio_output_channels.num_chmaps);
    }

    if (!has_video_track(mpctx))
        ao_flags |= AO_INIT_MEDIA_ROLE_MUSIC;

    mpctx->ao_filter_fmt = out_fmt;

    mpctx->ao = ao_init_best(
        mpctx->global, ao_flags, mp_wakeup_core_cb, mpctx, mpctx->encode_lavc_ctx, out_rate, out_format, out_channels);

    int ao_rate = 0;
    int ao_format = 0;
    struct mp_chmap ao_channels = { 0 };
    if (mpctx->ao)
        ao_get_format(mpctx->ao, &ao_rate, &ao_format, &ao_channels);

    // Verify passthrough format was not changed.
    if (mpctx->ao && af_fmt_is_spdif(out_format)) {
        if (out_rate != ao_rate || out_format != ao_format || !mp_chmap_equals(&out_channels, &ao_channels)) {
            MP_ERR(mpctx, "Passthrough format unsupported.\n");
            ao_uninit(mpctx->ao);
            mpctx->ao = NULL;
        }
    }

    if (!mpctx->ao) {
        // If spdif was used, try to fallback to PCM.
        if (spdif_fallback && ao_c->track && ao_c->track->dec) {
            MP_VERBOSE(mpctx, "Falling back to PCM output.\n");
            ao_c->spdif_passthrough = false;
            ao_c->spdif_failed = true;
            mp_decoder_wrapper_set_spdif_flag(ao_c->track->dec, false);
            if (!mp_decoder_wrapper_reinit(ao_c->track->dec))
                goto init_error;
            reset_audio_state(mpctx);
            mp_output_chain_reset_harder(ao_c->filter);
            mp_wakeup_core(mpctx); // reinit with new format next time
            return 0;
        }

        MP_ERR(mpctx, "Could not open/initialize audio device -> no sound.\n");
        mpctx->error_playing = MPV_ERROR_AO_INIT_FAILED;
        goto init_error;
    }

    char tmp[192];
    MP_INFO(mpctx, "AO: [%s] %s\n", ao_get_name(mpctx->ao),
        audio_config_to_str_buf(tmp, sizeof(tmp), ao_rate, ao_format, ao_channels));
    MP_VERBOSE(mpctx, "AO: Description: %s\n", ao_get_description(mpctx->ao));
    update_window_title(mpctx, true);

    ao_c->ao_resume_time = opts->audio_wait_open > 0 ? mp_time_sec() + opts->audio_wait_open : 0;

    bool eof = mpctx->audio_status == STATUS_EOF;
    ao_set_paused(mpctx->ao, get_internal_paused(mpctx), eof);

    ao_chain_set_ao(ao_c, mpctx->ao);

    audio_update_volume(mpctx);

    // Almost nonsensical hack to deal with certain format change scenarios.
    if (mpctx->audio_status == STATUS_PLAYING)
        ao_start(mpctx->ao);

    mp_wakeup_core(mpctx);
    mp_notify(mpctx, MPV_EVENT_AUDIO_RECONFIG, NULL);

    return 0;

init_error:
    uninit_audio_chain(mpctx);
    uninit_audio_out(mpctx);
    error_on_track(mpctx, track);
    return -1;
}

int init_audio_decoder(struct MPContext *mpctx, struct track *track)
{
    mp_assert(!track->dec);
    if (!track->stream)
        goto init_error;

    track->dec = mp_decoder_wrapper_create(mpctx->filter_root, track->stream);
    if (!track->dec)
        goto init_error;

    if (track->ao_c)
        mp_decoder_wrapper_set_spdif_flag(track->dec, true);

    if (!mp_decoder_wrapper_reinit(track->dec))
        goto init_error;

    return 1;

init_error:
    if (track->sink)
        mp_pin_disconnect(track->sink);
    track->sink = NULL;
    error_on_track(mpctx, track);
    return 0;
}

void reinit_audio_chain(struct MPContext *mpctx)
{
    struct track *track = NULL;
    track = mpctx->current_track[0][STREAM_AUDIO];
    if (!track || !track->stream) {
        if (!mpctx->encode_lavc_ctx)
            uninit_audio_out(mpctx);
        error_on_track(mpctx, track);
        return;
    }
    reinit_audio_chain_src(mpctx, track);
}

static const struct mp_filter_info ao_filter = {
    .name = "ao",
    .process = ao_process,
};

// (track=NULL creates a blank chain, used for lavfi-complex)
void reinit_audio_chain_src(struct MPContext *mpctx, struct track *track)
{
    mp_assert(!mpctx->ao_chain);

    mp_notify(mpctx, MPV_EVENT_AUDIO_RECONFIG, NULL);

    struct ao_chain *ao_c = talloc_zero(NULL, struct ao_chain);
    mpctx->ao_chain = ao_c;
    ao_c->mpctx = mpctx;
    ao_c->log = mpctx->log;
    ao_c->filter = mp_output_chain_create(mpctx->filter_root, MP_OUTPUT_CHAIN_AUDIO);
    ao_c->spdif_passthrough = true;
    ao_c->last_out_pts = MP_NOPTS_VALUE;
    ao_c->delay = mpctx->opts->audio_delay;

    ao_c->ao_filter = mp_filter_create(mpctx->filter_root, &ao_filter);
    if (!ao_c->filter || !ao_c->ao_filter)
        goto init_error;
    ao_c->ao_filter->priv = ao_c;

    mp_filter_add_pin(ao_c->ao_filter, MP_PIN_IN, "in");
    mp_pin_connect(ao_c->ao_filter->pins[0], ao_c->filter->f->pins[1]);

    if (track) {
        ao_c->track = track;
        track->ao_c = ao_c;
        if (!init_audio_decoder(mpctx, track))
            goto init_error;
        ao_c->dec_src = track->dec->f->pins[0];
        mp_pin_connect(ao_c->filter->f->pins[0], ao_c->dec_src);
    }

    reset_audio_state(mpctx);

    if (recreate_audio_filters(mpctx) < 0)
        goto init_error;

    if (mpctx->ao)
        audio_update_volume(mpctx);

    mp_wakeup_core(mpctx);
    return;

init_error:
    uninit_audio_chain(mpctx);
    uninit_audio_out(mpctx);
    error_on_track(mpctx, track);
}

// Return pts value corresponding to the start point of audio written to the
// ao queue so far.
double written_audio_pts(struct MPContext *mpctx)
{
    return mpctx->ao_chain ? mpctx->ao_chain->last_out_pts : MP_NOPTS_VALUE;
}

// Return pts value corresponding to currently playing audio adjusted for AO delay
// and playback speed.
double playing_audio_pts(struct MPContext *mpctx)
{
    double pts = written_audio_pts(mpctx);
    if (pts == MP_NOPTS_VALUE || !mpctx->ao)
        return pts;
    return pts - mpctx->audio_speed * ao_get_delay(mpctx->ao);
}

// This garbage is needed for untimed AOs. These consume audio infinitely fast,
// so try keeping approximate A/V sync by blocking audio transfer as needed.
static void update_throttle(struct MPContext *mpctx)
{
    struct ao_chain *ao_c = mpctx->ao_chain;
    bool new_throttle = mpctx->audio_status == STATUS_PLAYING && mpctx->delay > 0 && ao_c && ao_c->ao
        && ao_untimed(ao_c->ao) && mpctx->video_status != STATUS_EOF;
    if (ao_c && new_throttle != ao_c->untimed_throttle) {
        ao_c->untimed_throttle = new_throttle;
        mp_wakeup_core(mpctx);
        mp_filter_wakeup(ao_c->ao_filter);
    }
}

static void ao_process(struct mp_filter *f)
{
    struct ao_chain *ao_c = f->priv;
    struct MPContext *mpctx = ao_c->mpctx;

    if (!ao_c->queue_filter) {
        // This will eventually lead to the creation of the AO + queue, due
        // to how f_output_chain and AO management works.
        mp_pin_out_request_data(f->ppins[0]);
        // Check for EOF with no data case, which is a mess because everything
        // hates us.
        struct mp_frame frame = mp_pin_out_read(f->ppins[0]);
        if (frame.type == MP_FRAME_EOF) {
            MP_VERBOSE(mpctx, "got EOF with no data before it\n");
            ao_c->out_eof = true;
            mpctx->audio_status = STATUS_DRAINING;
            mp_wakeup_core(mpctx);
        } else if (frame.type) {
            mp_pin_out_unread(f->ppins[0], frame);
        }
        return;
    }

    // Due to mp_async_queue_set_notifier() this function is called when the
    // queue becomes full. This affects state changes in the normal playloop,
    // so wake it up. But avoid redundant wakeups during normal playback.
    if (mpctx->audio_status != STATUS_PLAYING && mp_async_queue_is_full(ao_c->ao_queue))
        mp_wakeup_core(mpctx);

    if (mpctx->audio_status == STATUS_SYNCING && !ao_c->start_pts_known)
        return;

    if (ao_c->untimed_throttle)
        return;

    if (!mp_pin_can_transfer_data(ao_c->queue_filter->pins[0], f->ppins[0]))
        return;

    struct mp_frame frame = mp_pin_out_read(f->ppins[0]);
    if (frame.type == MP_FRAME_AUDIO) {
        struct mp_aframe *af = frame.data;

        double endpts = get_play_end_pts(mpctx);
        if (endpts != MP_NOPTS_VALUE) {
            endpts *= mpctx->play_dir;
            // Avoid decoding and discarding the entire rest of the file.
            if (mp_aframe_get_pts(af) >= endpts) {
                mp_pin_out_unread(f->ppins[0], frame);
                if (!ao_c->out_eof) {
                    ao_c->out_eof = true;
                    mp_pin_in_write(ao_c->queue_filter->pins[0], MP_EOF_FRAME);
                }
                return;
            }
        }
        double startpts = mpctx->audio_status == STATUS_SYNCING ? ao_c->start_pts : MP_NOPTS_VALUE;
        mp_aframe_clip_timestamps(af, startpts, endpts);

        int samples = mp_aframe_get_size(af);
        if (!samples) {
            mp_filter_internal_mark_progress(f);
            mp_frame_unref(&frame);
            return;
        }

        ao_c->out_eof = false;

        if (mpctx->audio_status == STATUS_DRAINING || mpctx->audio_status == STATUS_EOF) {
            // If a new frame comes decoder/filter EOF, we should preferably
            // call get_sync_pts() again, which (at least in obscure situations)
            // may require us to wait a while until the sync PTS is known. Our
            // code sucks and can't deal with that, so jump through a hoop to
            // get things done in the correct order.
            mp_pin_out_unread(f->ppins[0], frame);
            ao_c->start_pts_known = false;
            mpctx->audio_status = STATUS_SYNCING;
            mp_wakeup_core(mpctx);
            MP_VERBOSE(mpctx, "new audio frame after EOF\n");
            return;
        }

        mpctx->shown_aframes += samples;
        double real_samplerate = mp_aframe_get_rate(af) / mpctx->audio_speed;
        if (mpctx->video_status != STATUS_EOF)
            mpctx->delay += samples / real_samplerate;
        ao_c->last_out_pts = mp_aframe_end_pts(af);
        update_throttle(mpctx);

        // Gapless case: the AO is still playing from previous file. It makes
        // no sense to wait, and in fact the "full queue" event we're waiting
        // for may never happen, so start immediately.
        // If the new audio starts "later" (big video sync offset), transfer
        // of data is stopped somewhere else.
        if (mpctx->audio_status == STATUS_SYNCING && ao_is_playing(ao_c->ao)) {
            mpctx->audio_status = STATUS_READY;
            mp_wakeup_core(mpctx);
            MP_VERBOSE(mpctx, "previous audio still playing; continuing\n");
        }

        mp_pin_in_write(ao_c->queue_filter->pins[0], frame);
    } else if (frame.type == MP_FRAME_EOF) {
        MP_VERBOSE(mpctx, "audio filter EOF\n");

        ao_c->out_eof = true;
        mp_wakeup_core(mpctx);

        mp_pin_in_write(ao_c->queue_filter->pins[0], frame);
        mp_filter_internal_mark_progress(f);
    } else {
        mp_frame_unref(&frame);
    }
}

void reload_audio_output(struct MPContext *mpctx)
{
    if (!mpctx->ao)
        return;

    ao_reset(mpctx->ao);
    uninit_audio_out(mpctx);
    reinit_audio_filters(mpctx); // mostly to issue refresh seek

    struct ao_chain *ao_c = mpctx->ao_chain;

    if (ao_c) {
        reset_audio_state(mpctx);
        mp_output_chain_reset_harder(ao_c->filter);
    }

    // Whether we can use spdif might have changed. If we failed to use spdif
    // in the previous initialization, try it with spdif again (we'll fallback
    // to PCM again if necessary).
    if (ao_c && ao_c->track) {
        struct mp_decoder_wrapper *dec = ao_c->track->dec;
        if (dec && ao_c->spdif_failed) {
            ao_c->spdif_passthrough = true;
            ao_c->spdif_failed = false;
            mp_decoder_wrapper_set_spdif_flag(ao_c->track->dec, true);
            if (!mp_decoder_wrapper_reinit(dec)) {
                MP_ERR(mpctx, "Error reinitializing audio.\n");
                error_on_track(mpctx, ao_c->track);
            }
        }
    }

    mp_wakeup_core(mpctx);
}

static void reset_starfish_audio_sync(struct MPContext *mpctx)
{
    bool was_active = mpctx->starfish_audio_sync_last_ns || mpctx->starfish_audio_sync_last_log_ns
        || mpctx->starfish_audio_sync_start_ns || mpctx->starfish_audio_sync_hard_since_ns
        || mpctx->starfish_audio_sync_avd_filtered != 0;
    if (!was_active)
        return;
    mpctx->starfish_audio_sync_last_ns = 0;
    mpctx->starfish_audio_sync_last_log_ns = 0;
    mpctx->starfish_audio_sync_start_ns = 0;
    mpctx->starfish_audio_sync_hard_since_ns = 0;
    mpctx->starfish_audio_sync_hard_direction = 0;
    mpctx->starfish_audio_sync_avd_filtered = 0;
    if (!mpctx->display_sync_active && mpctx->speed_factor_a != 1.0) {
        mpctx->speed_factor_a = 1.0;
        update_playback_speed(mpctx);
    }
}

void mark_starfish_audio_sync_seek(struct MPContext *mpctx)
{
    if (starfish_split_clock(mpctx)) {
        mpctx->starfish_audio_sync_resync_pending = true;
        mpctx->starfish_audio_sync_last_realign_ns = 0;
        mpctx->starfish_audio_clock_wait_logged = false;
        mpctx->starfish_audio_start_realign_done = false;
        reset_starfish_audio_sync(mpctx);
    }
}

void prepare_starfish_audio_resume(struct MPContext *mpctx)
{
    struct ao_chain *ao_c = mpctx->ao_chain;
    if (!starfish_split_clock(mpctx) || !ao_c || !ao_c->ao
        || mpctx->audio_status == STATUS_EOF)
        return;

    MP_VERBOSE(mpctx, "preparing ALSA for live Starfish resume\n");
    ao_reset(ao_c->ao);
    reset_audio_state(mpctx);
    mp_filter_wakeup(ao_c->ao_filter);
    mp_wakeup_core(mpctx);
}

static void sync_alsa_to_starfish_clock(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;

    if (!is_starfish_video_out(mpctx) || !is_alsa_audio_out(mpctx) || mpctx->display_sync_active
        || mpctx->audio_status != STATUS_PLAYING || mpctx->video_status != STATUS_PLAYING || get_internal_paused(mpctx)
        || mpctx->play_dir < 0) {
        reset_starfish_audio_sync(mpctx);
        return;
    }

    int64_t now = mp_time_ns();
    if (mpctx->starfish_audio_sync_last_ns
        && now - mpctx->starfish_audio_sync_last_ns < STARFISH_AUDIO_SYNC_PERIOD_NS) {
        double wait = MP_TIME_NS_TO_S(STARFISH_AUDIO_SYNC_PERIOD_NS - (now - mpctx->starfish_audio_sync_last_ns));
        mp_set_timeout(mpctx, wait);
        return;
    }

    double video_pts = MP_NOPTS_VALUE;
    double audio_pts = playing_audio_pts(mpctx);
    if (!query_external_video_clock(mpctx, &video_pts) || video_pts == MP_NOPTS_VALUE || audio_pts == MP_NOPTS_VALUE) {
        reset_starfish_audio_sync(mpctx);
        mp_set_timeout(mpctx, MP_TIME_NS_TO_S(STARFISH_AUDIO_SYNC_PERIOD_NS));
        return;
    }

    double av_diff = audio_pts - video_pts + opts->audio_delay;
    if (mpctx->starfish_audio_sync_resync_pending) {
        MP_VERBOSE(
            mpctx, "Starfish ALSA post-seek sync check audio=%f video=%f diff=%f\n", audio_pts, video_pts, av_diff);
        mpctx->starfish_audio_sync_resync_pending = false;
    }

    double elapsed = mpctx->starfish_audio_sync_last_ns ? MP_TIME_NS_TO_S(now - mpctx->starfish_audio_sync_last_ns)
                                                        : MP_TIME_NS_TO_S(STARFISH_AUDIO_SYNC_PERIOD_NS);
    mpctx->starfish_audio_sync_last_ns = now;

    if (!mpctx->starfish_audio_sync_start_ns) {
        mpctx->starfish_audio_sync_start_ns = now;
        mp_set_timeout(mpctx, MP_TIME_NS_TO_S(STARFISH_AUDIO_SYNC_PERIOD_NS));
        return;
    }

    if (now - mpctx->starfish_audio_sync_start_ns < STARFISH_AUDIO_SYNC_SETTLE_NS) {
        mpctx->starfish_audio_sync_avd_filtered = 0;
        mpctx->starfish_audio_sync_hard_since_ns = 0;
        mpctx->starfish_audio_sync_hard_direction = 0;
        mpctx->last_av_difference = 0;
        if (mpctx->speed_factor_a != 1.0) {
            mpctx->speed_factor_a = 1.0;
            update_playback_speed(mpctx);
        }
        if (!mpctx->starfish_audio_sync_last_log_ns
            || now - mpctx->starfish_audio_sync_last_log_ns >= STARFISH_AUDIO_SYNC_LOG_PERIOD_NS) {
            MP_VERBOSE(mpctx, "Starfish ALSA sync settling audio=%f video=%f\n", audio_pts, video_pts);
            mpctx->starfish_audio_sync_last_log_ns = now;
        }
        mp_set_timeout(mpctx, MP_TIME_NS_TO_S(STARFISH_AUDIO_SYNC_PERIOD_NS));
        return;
    }

    if (fabs(av_diff) >= STARFISH_AUDIO_SYNC_HARD_REALIGN_THRESHOLD) {
        int direction = av_diff > 0 ? 1 : -1;
        if (!mpctx->starfish_audio_sync_hard_since_ns || direction != mpctx->starfish_audio_sync_hard_direction) {
            mpctx->starfish_audio_sync_hard_since_ns = now;
            mpctx->starfish_audio_sync_hard_direction = direction;
        } else if (now - mpctx->starfish_audio_sync_hard_since_ns >= STARFISH_AUDIO_SYNC_HARD_REALIGN_CONFIRM_NS
            && (!mpctx->starfish_audio_sync_last_realign_ns
                || now - mpctx->starfish_audio_sync_last_realign_ns >= STARFISH_AUDIO_SYNC_HARD_REALIGN_COOLDOWN_NS)) {
            double target = video_pts - opts->audio_delay;
            MP_WARN(mpctx, "Starfish ALSA hard realign audio=%f video=%f diff=%f target=%f sustained=%.3fs\n",
                audio_pts, video_pts, av_diff, target, MP_TIME_NS_TO_S(now - mpctx->starfish_audio_sync_hard_since_ns));
            mpctx->starfish_audio_sync_last_realign_ns = now;
            mpctx->starfish_audio_sync_hard_since_ns = 0;
            mpctx->starfish_audio_sync_hard_direction = 0;
            realign_starfish_audio_start(mpctx, target);
            return;
        }
    } else {
        mpctx->starfish_audio_sync_hard_since_ns = 0;
        mpctx->starfish_audio_sync_hard_direction = 0;
    }

    double alpha = elapsed / (STARFISH_AUDIO_SYNC_FILTER_TIME + elapsed);
    mpctx->starfish_audio_sync_avd_filtered += alpha * (av_diff - mpctx->starfish_audio_sync_avd_filtered);
    mpctx->last_av_difference = mpctx->starfish_audio_sync_avd_filtered;

    double filtered = mpctx->starfish_audio_sync_avd_filtered;
    double phase_error = 0;
    if (fabs(filtered) > STARFISH_AUDIO_SYNC_DEADBAND) {
        phase_error = copysign(fabs(filtered) - STARFISH_AUDIO_SYNC_DEADBAND, filtered);
        double learn_scale = STARFISH_AUDIO_SYNC_RATE_LEARN_TIME * STARFISH_AUDIO_SYNC_RATE_LEARN_TIME;
        mpctx->starfish_audio_sync_rate_bias
            = MPCLAMP(mpctx->starfish_audio_sync_rate_bias - filtered * elapsed / learn_scale,
                -STARFISH_AUDIO_SYNC_RATE_BIAS_MAX, STARFISH_AUDIO_SYNC_RATE_BIAS_MAX);
    }

    double other = MPMAX(opts->playback_speed, 0.01);
    double correction
        = mpctx->starfish_audio_sync_rate_bias - phase_error / (STARFISH_AUDIO_SYNC_PHASE_RECOVERY_TIME * other);
    double max_correct = MPMAX(opts->sync_max_audio_change / 100.0, STARFISH_AUDIO_SYNC_CORRECTION_MAX);
    correction = MPCLAMP(correction, -max_correct, max_correct);
    double target = 1.0 + correction;
    double step = STARFISH_AUDIO_SYNC_SLEW_PER_SEC * MPMIN(elapsed, 0.25);
    double next = mpctx->speed_factor_a + MPCLAMP(target - mpctx->speed_factor_a, -step, step);

    if (fabs(next - mpctx->speed_factor_a) > 0.0000001) {
        mpctx->speed_factor_a = next;
        update_playback_speed(mpctx);
    }

    if (!mpctx->starfish_audio_sync_last_log_ns
        || now - mpctx->starfish_audio_sync_last_log_ns >= STARFISH_AUDIO_SYNC_LOG_PERIOD_NS) {
        MP_VERBOSE(mpctx, "Starfish ALSA sync audio=%f video=%f diff=%f filtered=%f speed=%f target=%f bias=%f\n",
            audio_pts, video_pts, av_diff, mpctx->starfish_audio_sync_avd_filtered, mpctx->speed_factor_a, target,
            mpctx->starfish_audio_sync_rate_bias);
        mpctx->starfish_audio_sync_last_log_ns = now;
    }

    mp_set_timeout(mpctx, MP_TIME_NS_TO_S(STARFISH_AUDIO_SYNC_PERIOD_NS));
}

static bool get_sync_pts(struct MPContext *mpctx, double *pts, bool require_live_starfish_clock)
{
    struct MPOpts *opts = mpctx->opts;
    bool starfish_video = is_starfish_video_out(mpctx);
    bool split_clock = starfish_split_clock(mpctx);

    *pts = MP_NOPTS_VALUE;

    if (starfish_video && mpctx->video_status != STATUS_EOF) {
        double audio_start_delay = split_clock ? opts->audio_delay : 0;
        double external_pts = MP_NOPTS_VALUE;
        if (split_clock && query_external_video_clock(mpctx, &external_pts)) {
            *pts = external_pts - audio_start_delay;
            return true;
        }

        if (!require_live_starfish_clock || !split_clock) {
            if (mpctx->hrseek_active && mpctx->hrseek_pts != MP_NOPTS_VALUE) {
                *pts = mpctx->hrseek_pts - audio_start_delay;
                return true;
            }
            if (split_clock && mpctx->playback_pts != MP_NOPTS_VALUE) {
                *pts = mpctx->playback_pts - audio_start_delay;
                return true;
            }
            if (mpctx->video_pts != MP_NOPTS_VALUE) {
                *pts = mpctx->video_pts - audio_start_delay;
                return true;
            }
            if (mpctx->playback_pts != MP_NOPTS_VALUE) {
                *pts = mpctx->playback_pts - audio_start_delay;
                return true;
            }
        }

        if (split_clock)
            mp_set_timeout(mpctx, MP_TIME_NS_TO_S(STARFISH_AUDIO_SYNC_PERIOD_NS));
        return false;
    }

    if (!opts->initial_audio_sync)
        return true;

    bool sync_to_video = mpctx->vo_chain && mpctx->video_status != STATUS_EOF && !mpctx->vo_chain->is_sparse;

    if (sync_to_video) {
        if (mpctx->video_status < STATUS_READY)
            return false; // wait until we know a video PTS
        if (mpctx->video_pts != MP_NOPTS_VALUE)
            *pts = mpctx->video_pts - opts->audio_delay;
    } else if (mpctx->hrseek_active) {
        *pts = mpctx->hrseek_pts;
    } else {
        // If audio-only is enabled mid-stream during playback, sync accordingly.
        *pts = mpctx->playback_pts;
    }

    return true;
}

static void realign_starfish_audio_start(struct MPContext *mpctx, double pts)
{
    struct ao_chain *ao_c = mpctx->ao_chain;
    if (!ao_c || !ao_c->ao)
        return;

    MP_VERBOSE(mpctx, "Starfish ALSA reclip queued audio to %f\n", pts);
    ao_reset(ao_c->ao);
    ao_chain_reset_state(ao_c);
    ao_c->start_pts_known = true;
    ao_c->start_pts = pts;
    mpctx->audio_status = STATUS_SYNCING;
    mpctx->delay = 0;
    mpctx->last_av_difference = 0;
    mpctx->logged_async_diff = -1;
    mpctx->starfish_audio_sync_last_ns = 0;
    mpctx->starfish_audio_sync_last_log_ns = 0;
    mpctx->starfish_audio_sync_start_ns = 0;
    mpctx->starfish_audio_sync_hard_since_ns = 0;
    mpctx->starfish_audio_sync_hard_direction = 0;
    mpctx->starfish_audio_sync_avd_filtered = 0;
    mpctx->starfish_audio_start_realign_done = true;
    if (!mpctx->display_sync_active && mpctx->speed_factor_a != 1.0) {
        mpctx->speed_factor_a = 1.0;
        update_playback_speed(mpctx);
    }
    mp_filter_wakeup(ao_c->ao_filter);
    mp_wakeup_core(mpctx);
}

// Look whether audio can be started yet - if audio has to start some time
// after video.
// Caller needs to ensure mpctx->restart_complete is OK
void audio_start_ao(struct MPContext *mpctx)
{
    struct ao_chain *ao_c = mpctx->ao_chain;
    if (!ao_c || !ao_c->ao || mpctx->audio_status != STATUS_READY)
        return;
    bool split_clock = starfish_split_clock(mpctx) && mpctx->video_status != STATUS_EOF;
    double pts = MP_NOPTS_VALUE;
    if (split_clock) {
        if (!get_sync_pts(mpctx, &pts, true)) {
            if (!mpctx->starfish_audio_clock_wait_logged) {
                MP_VERBOSE(mpctx, "waiting for live Starfish video clock before ALSA start\n");
                mpctx->starfish_audio_clock_wait_logged = true;
            }
            mp_set_timeout(mpctx, MP_TIME_NS_TO_S(STARFISH_AUDIO_SYNC_PERIOD_NS));
            return;
        }
        mpctx->starfish_audio_clock_wait_logged = false;
    } else if (!get_sync_pts(mpctx, &pts, false)) {
        return;
    }

    double desired_pts = pts;
    if (split_clock) {
        double external_pts = MP_NOPTS_VALUE;
        if (query_external_video_clock(mpctx, &external_pts))
            desired_pts = external_pts - mpctx->opts->audio_delay;
    }
    double apts = playing_audio_pts(mpctx);
    if (split_clock && !mpctx->starfish_audio_start_realign_done
        && desired_pts != MP_NOPTS_VALUE && apts != MP_NOPTS_VALUE
        && desired_pts > apts
        && desired_pts - apts > STARFISH_AUDIO_START_REALIGN_THRESHOLD) {
        realign_starfish_audio_start(mpctx, desired_pts);
        return;
    }
    if (desired_pts != MP_NOPTS_VALUE && apts != MP_NOPTS_VALUE
        && desired_pts < apts && mpctx->video_status != STATUS_EOF) {
        double diff = (apts - desired_pts) / mpctx->opts->playback_speed;
        if (!get_internal_paused(mpctx))
            mp_set_timeout(mpctx, diff);
        if (mpctx->logged_async_diff != diff) {
            MP_VERBOSE(mpctx, "delaying audio start %f vs. %f, diff=%f\n",
                       apts, desired_pts, diff);
            mpctx->logged_async_diff = diff;
            ao_c->delaying_audio_start = true;
        }
        return;
    }

    MP_VERBOSE(mpctx, "starting audio playback after live video clock\n");
    ao_c->delaying_audio_start = false;
    ao_start(ao_c->ao);
    mpctx->starfish_audio_clock_wait_logged = false;
    if (!split_clock)
        mpctx->starfish_audio_sync_resync_pending = false;
    mpctx->audio_status = STATUS_PLAYING;
    if (ao_c->out_eof) {
        mpctx->audio_status = STATUS_DRAINING;
        MP_VERBOSE(mpctx, "audio draining\n");
    }
    ao_c->underrun = false;
    mpctx->logged_async_diff = -1;
    mp_wakeup_core(mpctx);
}

void fill_audio_out_buffers(struct MPContext *mpctx)
{
    struct MPOpts *opts = mpctx->opts;

    if (mpctx->ao && ao_query_and_reset_events(mpctx->ao, AO_EVENT_RELOAD))
        reload_audio_output(mpctx);

    update_throttle(mpctx);

    struct ao_chain *ao_c = mpctx->ao_chain;
    if (!ao_c)
        return;

    if (ao_c->filter->failed_output_conversion) {
        error_on_track(mpctx, ao_c->track);
        return;
    }

    if (ao_c->filter->ao_needs_update) {
        if (reinit_audio_filters_and_output(mpctx) < 0)
            return;
    }

    if (mpctx->vo_chain && ao_c->track && ao_c->track->dec && mp_decoder_wrapper_get_pts_reset(ao_c->track->dec)) {
        MP_WARN(mpctx, "Reset playback due to audio timestamp reset.\n");
        reset_playback_state(mpctx);
        mp_wakeup_core(mpctx);
    }

    if (mpctx->audio_status == STATUS_SYNCING) {
        double pts;
        bool ok = get_sync_pts(mpctx, &pts, false);

        // If the AO is still playing from the previous file (due to gapless),
        // but if video is active, this may not work if audio starts later than
        // video, and gapless has no advantages anyway. So block doing anything
        // until the old audio is fully played.
        // (Buggy if AO underruns.)
        if (mpctx->ao && ao_is_playing(mpctx->ao)
            && mpctx->video_status != STATUS_EOF) {
            MP_VERBOSE(mpctx, "blocked, waiting for old audio to play\n");
            ok = false;
        }

        if (ao_c->start_pts_known != ok || ao_c->start_pts != pts) {
            ao_c->start_pts_known = ok;
            ao_c->start_pts = pts;
            mp_filter_wakeup(ao_c->ao_filter);
        }

        if (ao_c->ao && mp_async_queue_is_full(ao_c->ao_queue)) {
            mpctx->audio_status = STATUS_READY;
            mp_wakeup_core(mpctx);
            MP_VERBOSE(mpctx, "audio ready\n");
        } else if (ao_c->out_eof) {
            // Force playback start early.
            mpctx->audio_status = STATUS_READY;
            mp_wakeup_core(mpctx);
            MP_VERBOSE(mpctx, "audio ready (and EOF)\n");
        }
    }

    if (ao_c->ao && !ao_is_playing(ao_c->ao) && !ao_c->underrun
        && (mpctx->audio_status == STATUS_PLAYING || mpctx->audio_status == STATUS_DRAINING)) {
        // Should be playing, but somehow isn't.

        if (ao_c->out_eof && !mp_async_queue_get_frames(ao_c->ao_queue)) {
            MP_VERBOSE(mpctx, "AO signaled EOF (while in state %s)\n", mp_status_str(mpctx->audio_status));
            mpctx->audio_status = STATUS_EOF;
            mp_wakeup_core(mpctx);
            // stops untimed AOs, stops pull AOs from streaming silence
            ao_reset(ao_c->ao);
        } else {
            if (!ao_c->ao_underrun) {
                MP_WARN(mpctx, "Audio device underrun detected.\n");
                ao_c->ao_underrun = true;
                mp_wakeup_core(mpctx);
                ao_c->underrun = true;
            }

            // Wait until buffers are filled before recovering underrun.
            if (ao_c->out_eof || mp_async_queue_is_full(ao_c->ao_queue)) {
                MP_VERBOSE(mpctx, "restarting audio after underrun\n");
                ao_start(mpctx->ao_chain->ao);
                ao_c->ao_underrun = false;
                ao_c->underrun = false;
                mp_wakeup_core(mpctx);
            }
        }
    }

    if (mpctx->audio_status == STATUS_PLAYING && ao_c->out_eof) {
        mpctx->audio_status = STATUS_DRAINING;
        MP_VERBOSE(mpctx, "audio draining\n");
        mp_wakeup_core(mpctx);
    }

    if (mpctx->audio_status == STATUS_DRAINING) {
        // Wait until the AO has played all queued data. In the gapless case,
        // we trigger EOF immediately, and let it play asynchronously.
        if (!ao_c->ao || (!ao_is_playing(ao_c->ao) || opts->gapless_audio)) {
            MP_VERBOSE(mpctx, "audio EOF reached\n");
            mpctx->audio_status = STATUS_EOF;
            mp_wakeup_core(mpctx);
        }
    }

    sync_alsa_to_starfish_clock(mpctx);

    if (mpctx->restart_complete)
        audio_start_ao(mpctx); // in case it got delayed
}

// Drop data queued for output, or which the AO is currently outputting.
void clear_audio_output_buffers(struct MPContext *mpctx)
{
    if (mpctx->ao)
        ao_reset(mpctx->ao);
}
