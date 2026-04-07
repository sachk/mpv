#include "starfish_ctx.h"

#include <inttypes.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <appswitching-control-block/AcbAPI.h>
#include <player-factory/custompipeline.hpp>
#include <player-factory/customplayer.hpp>
#include <starfish-media-pipeline/StarfishMediaAPIs.h>

extern "C" {
#include <libavcodec/avcodec.h>

#include "audio/chmap.h"
#include "audio/format.h"
#include "common/av_common.h"
#include "common/common.h"
#include "common/msg.h"
#include "demux/stheader.h"
#include "mpv_talloc.h"
#include "video/hwdec.h"
}

#include "starfish_json.h"

namespace {

const char *get_app_id()
{
    const char *app_id = getenv("APPID");
    return app_id && app_id[0] ? app_id : "mpv";
}

const char *video_codec_name(enum AVCodecID codec)
{
    switch (codec) {
    case AV_CODEC_ID_VP8:
        return "VP8";
    case AV_CODEC_ID_VP9:
        return "VP9";
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_AVS:
    case AV_CODEC_ID_CAVS:
        return "H264";
    case AV_CODEC_ID_HEVC:
        return "H265";
    case AV_CODEC_ID_AV1:
        return "AV1";
    default:
        return NULL;
    }
}

const char *audio_codec_name_from_format(int format)
{
    switch (format) {
    case AF_FORMAT_S_AAC:
        return "AAC";
    case AF_FORMAT_S_AC3:
        return "AC3";
    case AF_FORMAT_S_EAC3:
        return "AC3 PLUS";
    case AF_FORMAT_S_DTS:
    case AF_FORMAT_S_DTSHD:
        return "DTS";
    case AF_FORMAT_S_MP3:
        return "MP3";
    case AF_FORMAT_S_TRUEHD:
        return "TRUEHD";
    default:
        return NULL;
    }
}

void acb_callback(long acbId, long taskId, long eventType, long appState,
                  long playState, const char *reply)
{
}

} // namespace

struct starfish_ctx {
    std::atomic<int> refs{1};
    struct mp_log *log = nullptr;
    std::mutex lock;
    std::unique_ptr<StarfishMediaAPIs> media;
    std::string window_id;
    std::string video_codec;
    std::string audio_codec;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int fps_num = 0;
    int fps_den = 1;
    bool need_audio = false;
    bool loaded = false;
    bool load_requested = false;
    bool play_requested = false;
    bool eos_sent = false;
    bool eos_pending = false;
    bool ended = false;
    long acb_id = 0;
    long acb_task_id = 0;
    bool acb_initialized = false;
    std::deque<struct starfish_video_frame> ready_frames;
    starfish_wakeup_cb video_wakeup = nullptr;
    void *video_wakeup_opaque = nullptr;
    starfish_wakeup_cb audio_wakeup = nullptr;
    void *audio_wakeup_opaque = nullptr;
};

static std::mutex g_current_lock;
static struct starfish_ctx *g_current_ctx;
static void player_callback(int32_t type, int64_t numValue, const char *strValue,
                            void *opaque);

static void wake_stream(struct starfish_ctx *ctx, enum starfish_stream_type stream)
{
    starfish_wakeup_cb cb = nullptr;
    void *opaque = nullptr;
    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        if (stream == STARFISH_STREAM_VIDEO) {
            cb = ctx->video_wakeup;
            opaque = ctx->video_wakeup_opaque;
        } else {
            cb = ctx->audio_wakeup;
            opaque = ctx->audio_wakeup_opaque;
        }
    }
    if (cb)
        cb(opaque);
}

static bool ensure_media(struct starfish_ctx *ctx)
{
    if (ctx->media)
        return true;
    ctx->media = std::make_unique<StarfishMediaAPIs>();
    return !!ctx->media;
}

static bool ensure_acb(struct starfish_ctx *ctx)
{
    if (!ctx->window_id.empty() || ctx->acb_id)
        return true;

    ctx->acb_id = AcbAPI_create();
    if (!ctx->acb_id) {
        mp_err(ctx->log, "AcbAPI_create failed\n");
        return false;
    }

    if (!AcbAPI_initialize(ctx->acb_id, PLAYER_TYPE_MSE, get_app_id(), &acb_callback)) {
        mp_err(ctx->log, "AcbAPI_initialize failed\n");
        AcbAPI_destroy(ctx->acb_id);
        ctx->acb_id = 0;
        return false;
    }

    ctx->acb_initialized = true;
    return true;
}

static int maybe_start_load(struct starfish_ctx *ctx)
{
    struct starfish_json_load_params params = {};
    bool have_window = false;

    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        if (ctx->loaded)
            return STARFISH_FEED_OK;
        if (ctx->load_requested)
            return STARFISH_FEED_AGAIN;
        if (ctx->video_codec.empty() || ctx->width <= 0 || ctx->height <= 0) {
            mp_err(ctx->log, "Starfish video pipeline not configured yet\n");
            return STARFISH_FEED_ERROR;
        }
        if (!ensure_media(ctx))
            return STARFISH_FEED_ERROR;
        have_window = !ctx->window_id.empty();
        if (!have_window && !ensure_acb(ctx))
            return STARFISH_FEED_ERROR;

        params = (struct starfish_json_load_params){
            .app_id = get_app_id(),
            .window_id = have_window ? ctx->window_id.c_str() : nullptr,
            .video_codec = ctx->video_codec.c_str(),
            .audio_codec = ctx->need_audio ? ctx->audio_codec.c_str() : nullptr,
            .width = ctx->width,
            .height = ctx->height,
            .fps_num = ctx->fps_num,
            .fps_den = ctx->fps_den,
            .pts_to_decode_ns = 0,
            .need_audio = ctx->need_audio,
        };
        ctx->load_requested = true;
        ctx->play_requested = true;
        ctx->ended = false;
        ctx->eos_sent = false;
        ctx->ready_frames.clear();
    }

    if (!ctx->media->notifyForeground())
        mp_warn(ctx->log, "Starfish notifyForeground failed\n");

    std::string payload = starfish_json_build_load(&params);
    if (!ctx->media->Load(payload.c_str(), &player_callback, ctx)) {
        std::lock_guard<std::mutex> lock(ctx->lock);
        ctx->load_requested = false;
        mp_err(ctx->log, "Starfish Load failed\n");
        return STARFISH_FEED_ERROR;
    }

    return STARFISH_FEED_AGAIN;
}

static void player_callback(int32_t type, int64_t numValue, const char *strValue, void *opaque)
{
    struct starfish_ctx *ctx = static_cast<struct starfish_ctx *>(opaque);
    bool wake_video = false;
    bool wake_audio = false;

    switch (type) {
    case PF_EVENT_TYPE_FRAMEREADY: {
        struct starfish_video_frame frame = {
            .pts = numValue / 1000000000.0,
            .dts = numValue / 1000000000.0,
            .duration = ctx->fps > 0 ? 1.0 / ctx->fps : 0.0,
        };
        {
            std::lock_guard<std::mutex> lock(ctx->lock);
            ctx->ready_frames.push_back(frame);
        }
        wake_video = true;
        break;
    }
    case PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED:
        {
            std::lock_guard<std::mutex> lock(ctx->lock);
            ctx->loaded = true;
            ctx->load_requested = false;
        }
        if (ctx->acb_id) {
            AcbAPI_setSinkType(ctx->acb_id, SINK_TYPE_MAIN);
            AcbAPI_setMediaId(ctx->acb_id, ctx->media->getMediaID());
            AcbAPI_setState(ctx->acb_id, APPSTATE_FOREGROUND, PLAYSTATE_LOADED,
                            &ctx->acb_task_id);
        }
        if (ctx->play_requested && !ctx->media->Play())
            mp_err(ctx->log, "Starfish Play failed after load\n");
        if (ctx->eos_pending && ctx->media->pushEOS()) {
            ctx->eos_sent = true;
            ctx->eos_pending = false;
        }
        wake_video = true;
        wake_audio = true;
        break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__PLAYING:
        if (ctx->acb_id) {
            AcbAPI_setState(ctx->acb_id, APPSTATE_FOREGROUND, PLAYSTATE_PLAYING,
                            &ctx->acb_task_id);
        }
        wake_video = true;
        wake_audio = true;
        break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__PAUSED:
        if (ctx->acb_id) {
            AcbAPI_setState(ctx->acb_id, APPSTATE_FOREGROUND, PLAYSTATE_PAUSED,
                            &ctx->acb_task_id);
        }
        wake_video = true;
        wake_audio = true;
        break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__ENDOFSTREAM:
        {
            std::lock_guard<std::mutex> lock(ctx->lock);
            ctx->ended = true;
        }
        wake_video = true;
        wake_audio = true;
        break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__UNLOADCOMPLETED:
        {
            std::lock_guard<std::mutex> lock(ctx->lock);
            ctx->loaded = false;
            ctx->load_requested = false;
            ctx->ended = true;
            ctx->ready_frames.clear();
        }
        if (ctx->acb_id) {
            AcbAPI_setState(ctx->acb_id, APPSTATE_FOREGROUND, PLAYSTATE_UNLOADED,
                            &ctx->acb_task_id);
        }
        wake_video = true;
        wake_audio = true;
        break;
    case PF_EVENT_TYPE_STR_BUFFERFULL:
        wake_video = true;
        wake_audio = true;
        break;
    case PF_EVENT_TYPE_STR_AUDIO_INFO:
        if (ctx->acb_id && strValue)
            AcbAPI_setMediaAudioData(ctx->acb_id, strValue, &ctx->acb_task_id);
        break;
    case PF_EVENT_TYPE_STR_VIDEO_INFO:
        if (ctx->acb_id && strValue)
            AcbAPI_setMediaVideoData(ctx->acb_id, strValue, &ctx->acb_task_id);
        break;
    case PF_EVENT_TYPE_INT_ERROR:
    case PF_EVENT_TYPE_STR_ERROR:
        mp_err(ctx->log, "Starfish callback error type=%d num=%" PRId64 " str=%s\n",
               type, numValue, strValue ? strValue : "");
        wake_video = true;
        wake_audio = true;
        break;
    default:
        break;
    }

    if (wake_video)
        wake_stream(ctx, STARFISH_STREAM_VIDEO);
    if (wake_audio)
        wake_stream(ctx, STARFISH_STREAM_AUDIO);
}

struct starfish_ctx *starfish_ctx_create(struct mp_log *log)
{
    struct starfish_ctx *ctx = new starfish_ctx();
    ctx->log = mp_log_new(NULL, log, "starfish");
    return ctx;
}

struct starfish_ctx *starfish_ctx_retain(struct starfish_ctx *ctx)
{
    if (ctx)
        ctx->refs.fetch_add(1, std::memory_order_relaxed);
    return ctx;
}

void starfish_ctx_unref(struct starfish_ctx *ctx)
{
    if (!ctx)
        return;
    if (ctx->refs.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;
    if (ctx->media)
        ctx->media->Unload();
    if (ctx->acb_id) {
        if (ctx->acb_initialized)
            AcbAPI_finalize(ctx->acb_id);
        AcbAPI_destroy(ctx->acb_id);
    }
    talloc_free(ctx->log);
    delete ctx;
}

struct starfish_ctx *starfish_ctx_from_hwdec(struct mp_hwdec_ctx *hwctx)
{
    return hwctx ? (struct starfish_ctx *)hwctx->conversion_config : NULL;
}

bool starfish_ctx_set_current(struct starfish_ctx *ctx)
{
    std::lock_guard<std::mutex> lock(g_current_lock);
    if (ctx == g_current_ctx)
        return true;
    starfish_ctx_retain(ctx);
    starfish_ctx_unref(g_current_ctx);
    g_current_ctx = ctx;
    return true;
}

struct starfish_ctx *starfish_ctx_get_current(void)
{
    std::lock_guard<std::mutex> lock(g_current_lock);
    return starfish_ctx_retain(g_current_ctx);
}

void starfish_ctx_set_wakeup_cb(struct starfish_ctx *ctx, enum starfish_stream_type stream,
                                starfish_wakeup_cb cb, void *opaque)
{
    std::lock_guard<std::mutex> lock(ctx->lock);
    if (stream == STARFISH_STREAM_VIDEO) {
        ctx->video_wakeup = cb;
        ctx->video_wakeup_opaque = opaque;
    } else {
        ctx->audio_wakeup = cb;
        ctx->audio_wakeup_opaque = opaque;
    }
}

bool starfish_ctx_set_window_id(struct starfish_ctx *ctx, const char *window_id)
{
    std::lock_guard<std::mutex> lock(ctx->lock);
    if (ctx->loaded || ctx->load_requested)
        return false;
    ctx->window_id = window_id ? window_id : "";
    return true;
}

bool starfish_ctx_set_numeric_window_id(struct starfish_ctx *ctx, int64_t wid)
{
    char wid_buf[32];

    if (wid <= 0)
        return false;
    snprintf(wid_buf, sizeof(wid_buf), "%" PRId64, wid);
    return starfish_ctx_set_window_id(ctx, wid_buf);
}

bool starfish_ctx_set_video_geometry(struct starfish_ctx *ctx, int width, int height,
                                     double fps)
{
    std::lock_guard<std::mutex> lock(ctx->lock);
    if (ctx->loaded || ctx->load_requested)
        return false;
    ctx->width = width;
    ctx->height = height;
    ctx->fps = fps;
    if (fps > 0) {
        ctx->fps_num = (int)(fps * 1000.0 + 0.5);
        ctx->fps_den = 1000;
    } else {
        ctx->fps_num = 0;
        ctx->fps_den = 1;
    }
    return true;
}

bool starfish_ctx_configure_video(struct starfish_ctx *ctx,
                                  const struct mp_codec_params *codec)
{
    const char *name = video_codec_name((enum AVCodecID)mp_codec_to_av_codec_id(codec->codec));
    if (!name)
        return false;

    std::lock_guard<std::mutex> lock(ctx->lock);
    if (ctx->loaded || ctx->load_requested)
        return false;
    ctx->video_codec = name;
    if (codec->lav_codecpar) {
        ctx->width = codec->lav_codecpar->width;
        ctx->height = codec->lav_codecpar->height;
    }
    if (codec->fps > 0.0 && ctx->fps <= 0.0) {
        ctx->fps = codec->fps;
        ctx->fps_num = (int)(codec->fps * 1000.0 + 0.5);
        ctx->fps_den = 1000;
    }
    return true;
}

bool starfish_ctx_configure_audio_passthrough(struct starfish_ctx *ctx, int format,
                                              int samplerate,
                                              const struct mp_chmap *channels)
{
    const char *name = audio_codec_name_from_format(format);
    if (!name)
        return false;

    std::lock_guard<std::mutex> lock(ctx->lock);
    if (ctx->loaded || ctx->load_requested)
        return false;
    ctx->audio_codec = name;
    ctx->need_audio = true;
    return true;
}

int starfish_ctx_feed_video(struct starfish_ctx *ctx, const void *data, size_t size,
                            double pts)
{
    int load_status = maybe_start_load(ctx);
    if (load_status != STARFISH_FEED_OK)
        return load_status;

    const int64_t pts_ns = pts == MP_NOPTS_VALUE ? 0 : (int64_t)(pts * 1e9);
    std::string payload = starfish_json_build_feed(STARFISH_STREAM_VIDEO, data, size, pts_ns);
    std::string result = ctx->media->Feed(payload.c_str());
    if (result.find("Ok") != std::string::npos)
        return STARFISH_FEED_OK;
    if (result.find("BufferFull") != std::string::npos)
        return STARFISH_FEED_AGAIN;

    mp_err(ctx->log, "Starfish video Feed failed: %s\n", result.c_str());
    return STARFISH_FEED_ERROR;
}

int starfish_ctx_feed_audio(struct starfish_ctx *ctx, const void *data, size_t size,
                            int64_t pts_ns)
{
    int load_status = maybe_start_load(ctx);
    if (load_status != STARFISH_FEED_OK)
        return load_status;

    std::string payload = starfish_json_build_feed(STARFISH_STREAM_AUDIO, data, size, pts_ns);
    std::string result = ctx->media->Feed(payload.c_str());
    if (result.find("Ok") != std::string::npos)
        return STARFISH_FEED_OK;
    if (result.find("BufferFull") != std::string::npos)
        return STARFISH_FEED_AGAIN;

    mp_err(ctx->log, "Starfish audio Feed failed: %s\n", result.c_str());
    return STARFISH_FEED_ERROR;
}

bool starfish_ctx_pop_video_frame(struct starfish_ctx *ctx,
                                  struct starfish_video_frame *frame)
{
    std::lock_guard<std::mutex> lock(ctx->lock);
    if (ctx->ready_frames.empty())
        return false;
    *frame = ctx->ready_frames.front();
    ctx->ready_frames.pop_front();
    return true;
}

bool starfish_ctx_resume(struct starfish_ctx *ctx)
{
    bool loaded = false;
    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        ctx->play_requested = true;
        loaded = ctx->loaded;
    }
    return !loaded || ctx->media->Play();
}

bool starfish_ctx_pause(struct starfish_ctx *ctx)
{
    bool loaded = false;
    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        ctx->play_requested = false;
        loaded = ctx->loaded;
    }
    return !loaded || ctx->media->Pause();
}

bool starfish_ctx_flush(struct starfish_ctx *ctx, double pts)
{
    bool loaded = false;
    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        ctx->ready_frames.clear();
        ctx->ended = false;
        ctx->eos_sent = false;
        ctx->eos_pending = false;
        loaded = ctx->loaded;
    }
    if (!loaded)
        return true;

    const int64_t pts_ns = pts == MP_NOPTS_VALUE ? 0 : (int64_t)(pts * 1e9);
    std::string payload = starfish_json_build_seek(pts_ns);
    if (!ctx->media->setTimeToDecode(payload.c_str()))
        return false;

    auto *player = static_cast<mediapipeline::CustomPlayer *>(ctx->media->player.get());
    auto *pipeline =
        static_cast<mediapipeline::CustomPipeline *>(player->getPipeline().get());
    pipeline->sendSegmentEvent();
    return true;
}

bool starfish_ctx_push_eos(struct starfish_ctx *ctx)
{
    bool do_push = false;
    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        if (ctx->loaded && !ctx->eos_sent) {
            ctx->eos_sent = true;
            do_push = true;
        } else if (!ctx->loaded) {
            ctx->eos_pending = true;
        }
    }
    return !do_push || ctx->media->pushEOS();
}

bool starfish_ctx_has_ended(struct starfish_ctx *ctx)
{
    std::lock_guard<std::mutex> lock(ctx->lock);
    return ctx->ended && ctx->ready_frames.empty();
}

int starfish_ctx_get_video_width(struct starfish_ctx *ctx)
{
    std::lock_guard<std::mutex> lock(ctx->lock);
    return ctx->width;
}

int starfish_ctx_get_video_height(struct starfish_ctx *ctx)
{
    std::lock_guard<std::mutex> lock(ctx->lock);
    return ctx->height;
}

double starfish_ctx_get_video_fps(struct starfish_ctx *ctx)
{
    std::lock_guard<std::mutex> lock(ctx->lock);
    return ctx->fps;
}
