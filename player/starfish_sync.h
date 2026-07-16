#pragma once

#include <string.h>

#include "audio/out/ao.h"
#include "options/options.h"
#include "osdep/timer.h"
#include "video/out/vo.h"

#include "core.h"

static inline bool is_starfish_video_out(struct MPContext *mpctx)
{
    return mpctx->video_out && mpctx->video_out->driver &&
           strcmp(mpctx->video_out->driver->name, "starfish") == 0;
}

static inline const char *active_audio_out_name(struct MPContext *mpctx)
{
    struct ao *ao = mpctx->ao_chain && mpctx->ao_chain->ao
        ? mpctx->ao_chain->ao : mpctx->ao;
    return ao ? ao_get_name(ao) : NULL;
}

static inline bool is_alsa_audio_out(struct MPContext *mpctx)
{
    const char *name = active_audio_out_name(mpctx);
    return name && strcmp(name, "alsa") == 0;
}

static inline bool is_starfish_audio_out(struct MPContext *mpctx)
{
    const char *name = active_audio_out_name(mpctx);
    return name && strcmp(name, "starfish") == 0;
}

static inline bool starfish_split_clock(struct MPContext *mpctx)
{
    return is_starfish_video_out(mpctx) && is_alsa_audio_out(mpctx);
}

static inline bool query_external_video_clock(struct MPContext *mpctx,
                                              double *pts_out)
{
    if (!mpctx->video_out)
        return false;
    struct voctrl_external_video_clock clock = {0};
    if (vo_control(mpctx->video_out, VOCTRL_GET_EXTERNAL_VIDEO_CLOCK, &clock)
        != VO_TRUE)
        return false;
    if (clock.pts == MP_NOPTS_VALUE || clock.host_time_ns <= 0)
        return false;
    double age = MP_TIME_NS_TO_S(mp_time_ns() - clock.host_time_ns);
    if (age < 0 || age > 0.250)
        return false;
    *pts_out = clock.pts + age * mpctx->opts->playback_speed;
    return true;
}
