#include "starfish_ctx.h"

#include <inttypes.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <glib.h>
#include <appswitching-control-block/AcbAPI.h>
#include <player-factory/custompipeline.hpp>
#include <player-factory/customplayer.hpp>
#include <starfish-media-pipeline/StarfishMediaAPIs.h>

extern "C" {
#define _Atomic
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>

#include "audio/chmap.h"
#include "audio/format.h"
#include "common/av_common.h"
#include "common/common.h"
#include "common/msg.h"
#include "demux/stheader.h"
#include "mpv_talloc.h"
#include "video/hwdec.h"
#undef _Atomic
}

#include "starfish_json.h"

namespace {

constexpr size_t VIDEO_QUEUE_LIMIT = 8 * 1024 * 1024;
constexpr size_t AUDIO_QUEUE_LIMIT = 2 * 1024 * 1024;
constexpr size_t VIDEO_INFLIGHT_LIMIT = 8 * 1024 * 1024;
constexpr size_t AUDIO_INFLIGHT_LIMIT = 2 * 1024 * 1024;

enum class pipeline_state {
    IDLE,
    WAIT_WINDOW,
    LOADING,
    LOADED,
    PLAYING,
    PAUSED,
    FAILED,
};

struct queued_packet {
    std::shared_ptr<std::vector<uint8_t>> data;
    int64_t pts_ns = 0;
};

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
        return nullptr;
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
        return nullptr;
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
    std::condition_variable cv;
    std::thread worker;
    bool stop = false;

    std::unique_ptr<StarfishMediaAPIs> media;

    std::string window_id;
    std::string video_codec;
    std::string audio_codec;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int fps_num = 0;
    int fps_den = 1;
    int max_width = 0;
    int max_height = 0;
    int max_framerate = 0;
    bool adaptive_resolution = false;
    bool need_audio = false;

    pipeline_state state = pipeline_state::IDLE;
    bool play_requested = true;
    bool need_segment = false;
    bool flush_requested = false;
    int64_t flush_pts_ns = 0;
    bool eos_sent = false;
    bool eos_pending = false;
    bool ended = false;
    long acb_id = 0;
    long acb_task_id = 0;
    bool acb_initialized = false;

    std::deque<queued_packet> video_queue;
    std::deque<queued_packet> audio_queue;
    std::deque<queued_packet> video_inflight;
    std::deque<queued_packet> audio_inflight;
    size_t video_queue_bytes = 0;
    size_t audio_queue_bytes = 0;
    size_t video_inflight_bytes = 0;
    size_t audio_inflight_bytes = 0;
    std::deque<struct starfish_video_frame> ready_frames;

    starfish_wakeup_cb video_wakeup = nullptr;
    void *video_wakeup_opaque = nullptr;
    starfish_wakeup_cb audio_wakeup = nullptr;
    void *audio_wakeup_opaque = nullptr;

    int failed_code = 0;
    std::string failed_reason;
};

static std::mutex g_current_lock;
static struct starfish_ctx *g_current_ctx;
static void player_callback(int32_t type, int64_t numValue, const char *strValue,
                            void *opaque);
static void worker_loop(struct starfish_ctx *ctx);

static const char *event_name(int32_t type)
{
    switch (type) {
    case PF_EVENT_TYPE_FRAMEREADY:
        return "FRAMEREADY";
    case PF_EVENT_TYPE_STR_VIDEO_INFO:
        return "STR_VIDEO_INFO";
    case PF_EVENT_TYPE_STR_VIDEO_TRACK_INFO:
        return "STR_VIDEO_TRACK_INFO";
    case PF_EVENT_TYPE_STR_AUDIO_INFO:
        return "STR_AUDIO_INFO";
    case PF_EVENT_TYPE_STR_AUDIO_TRACK_INFO:
        return "STR_AUDIO_TRACK_INFO";
    case PF_EVENT_TYPE_STR_ERROR:
        return "STR_ERROR";
    case PF_EVENT_TYPE_INT_ERROR:
        return "INT_ERROR";
    case PF_EVENT_TYPE_STR_STATE_UPDATE__PRELOADCOMPLETED:
        return "PRELOADCOMPLETED";
    case PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED:
        return "LOADCOMPLETED";
    case PF_EVENT_TYPE_STR_STATE_UPDATE__UNLOADCOMPLETED:
        return "UNLOADCOMPLETED";
    case PF_EVENT_TYPE_STR_STATE_UPDATE__PLAYING:
        return "PLAYING";
    case PF_EVENT_TYPE_STR_STATE_UPDATE__PAUSED:
        return "PAUSED";
    case PF_EVENT_TYPE_STR_STATE_UPDATE__ENDOFSTREAM:
        return "ENDOFSTREAM";
    case PF_EVENT_TYPE_STR_BUFFERFULL:
        return "BUFFERFULL";
    case PF_EVENT_TYPE_STR_BUFFERLOW:
        return "BUFFERLOW";
    case PF_EVENT_TYPE_INT_NUM_PROGRAM:
        return "NUM_PROGRAM";
    case PF_EVENT_TYPE_INT_NUM_VIDEO_TRACK:
        return "NUM_VIDEO_TRACK";
    case PF_EVENT_TYPE_INT_NUM_AUDIO_TRACK:
        return "NUM_AUDIO_TRACK";
    default:
        return "UNKNOWN";
    }
}

static bool is_loaded_state(pipeline_state state)
{
    return state == pipeline_state::LOADED ||
           state == pipeline_state::PLAYING ||
           state == pipeline_state::PAUSED;
}

static void wake_stream_locked(struct starfish_ctx *ctx, enum starfish_stream_type stream,
                               starfish_wakeup_cb *cb, void **opaque)
{
    if (stream == STARFISH_STREAM_VIDEO) {
        *cb = ctx->video_wakeup;
        *opaque = ctx->video_wakeup_opaque;
    } else {
        *cb = ctx->audio_wakeup;
        *opaque = ctx->audio_wakeup_opaque;
    }
}

static void wake_stream(struct starfish_ctx *ctx, enum starfish_stream_type stream)
{
    starfish_wakeup_cb cb = nullptr;
    void *opaque = nullptr;
    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        wake_stream_locked(ctx, stream, &cb, &opaque);
    }
    if (cb)
        cb(opaque);
}

static void wake_all(struct starfish_ctx *ctx)
{
    wake_stream(ctx, STARFISH_STREAM_VIDEO);
    wake_stream(ctx, STARFISH_STREAM_AUDIO);
}

static bool ensure_media(struct starfish_ctx *ctx)
{
    if (ctx->media)
        return true;
    ctx->media = std::make_unique<StarfishMediaAPIs>();
    return !!ctx->media;
}

static void update_adaptive_caps(struct starfish_ctx *ctx)
{
    if (ctx->video_codec.empty()) {
        ctx->adaptive_resolution = false;
        ctx->max_width = 0;
        ctx->max_height = 0;
        ctx->max_framerate = 0;
        return;
    }

    ctx->adaptive_resolution = false;
    ctx->max_width = 0;
    ctx->max_height = 0;
    ctx->max_framerate = 0;
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

static bool have_load_config_locked(struct starfish_ctx *ctx)
{
    return !ctx->video_codec.empty() && ctx->width > 0 && ctx->height > 0 &&
           ctx->fps_num > 0 && ctx->fps_den > 0;
}

static bool should_start_load_locked(struct starfish_ctx *ctx)
{
    if (ctx->state != pipeline_state::IDLE && ctx->state != pipeline_state::WAIT_WINDOW)
        return false;
    if (!have_load_config_locked(ctx) || ctx->video_queue.empty())
        return false;
    if (!ctx->window_id.empty())
        return true;
    return ensure_acb(ctx);
}

static void set_failed_locked(struct starfish_ctx *ctx, int code, const char *reason)
{
    ctx->state = pipeline_state::FAILED;
    ctx->failed_code = code;
    ctx->failed_reason = reason ? reason : "";
}

static bool set_time_to_decode(struct starfish_ctx *ctx, int64_t pts_ns)
{
    std::string payload = starfish_json_build_seek(pts_ns);
    if (ctx->media->setTimeToDecode(payload.c_str()))
        return true;

    auto *player = static_cast<mediapipeline::CustomPlayer *>(ctx->media->player.get());
    auto *pipeline = player ? static_cast<mediapipeline::CustomPipeline *>(player->getPipeline().get())
                            : nullptr;
    if (!pipeline)
        return false;

    MEDIA_CUSTOM_CONTENT_INFO_T content_info;
    pipeline->loadSpi_getInfo(&content_info);
    content_info.ptsToDecode = pts_ns;
    pipeline->setContentInfo(MEDIA_CUSTOM_SRC_TYPE_ES, &content_info);
    return false;
}

static bool try_feed_packet(struct starfish_ctx *ctx, enum starfish_stream_type stream,
                            const queued_packet &packet, bool *buffer_full)
{
    if (stream == STARFISH_STREAM_VIDEO && ctx->need_segment) {
        set_time_to_decode(ctx, packet.pts_ns);
        auto *player = static_cast<mediapipeline::CustomPlayer *>(ctx->media->player.get());
        auto *pipeline =
            player ? static_cast<mediapipeline::CustomPipeline *>(player->getPipeline().get())
                   : nullptr;
        if (pipeline)
            pipeline->sendSegmentEvent();
        ctx->need_segment = false;
    }

    std::string payload = starfish_json_build_feed(stream, packet.data->data(),
                                                   packet.data->size(), packet.pts_ns);
    std::string result = ctx->media->Feed(payload.c_str());
    mp_info(ctx->log, "Starfish %s feed: size=%zu pts=%" PRId64 " result=%s\n",
            stream == STARFISH_STREAM_VIDEO ? "video" : "audio",
            packet.data->size(), packet.pts_ns, result.c_str());

    if (result.find("Ok") != std::string::npos)
        return true;

    if (result.find("BufferFull") != std::string::npos) {
        *buffer_full = true;
        return false;
    }

    mp_warn(ctx->log, "Starfish %s Feed returned %s; retrying\n",
            stream == STARFISH_STREAM_VIDEO ? "video" : "audio",
            result.empty() ? "(empty)" : result.c_str());
    return false;
}

static void apply_flush(struct starfish_ctx *ctx)
{
    bool loaded = false;
    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        loaded = is_loaded_state(ctx->state);
    }

    if (loaded && !ctx->media->flush())
        mp_warn(ctx->log, "Starfish flush() failed\n");

    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        ctx->video_queue.clear();
        ctx->audio_queue.clear();
        ctx->video_inflight.clear();
        ctx->audio_inflight.clear();
        ctx->video_queue_bytes = 0;
        ctx->audio_queue_bytes = 0;
        ctx->video_inflight_bytes = 0;
        ctx->audio_inflight_bytes = 0;
        ctx->ready_frames.clear();
        ctx->ended = false;
        ctx->eos_sent = false;
        ctx->eos_pending = false;
        ctx->need_segment = true;
        ctx->flush_requested = false;
    }
}

static void worker_loop(struct starfish_ctx *ctx)
{
    std::unique_lock<std::mutex> lock(ctx->lock);

    while (!ctx->stop) {
        if (ctx->flush_requested) {
            lock.unlock();
            apply_flush(ctx);
            lock.lock();
            continue;
        }

        if (should_start_load_locked(ctx)) {
            struct starfish_json_load_params params = {
                .app_id = get_app_id(),
                .window_id = ctx->window_id.empty() ? nullptr : ctx->window_id.c_str(),
                .video_codec = ctx->video_codec.c_str(),
                .audio_codec = ctx->need_audio ? ctx->audio_codec.c_str() : nullptr,
                .width = ctx->width,
                .height = ctx->height,
                .fps_num = ctx->fps_num,
                .fps_den = ctx->fps_den,
                .max_width = ctx->max_width,
                .max_height = ctx->max_height,
                .max_framerate = ctx->max_framerate,
                .adaptive_resolution = ctx->adaptive_resolution,
                .pts_to_decode_ns = 0,
                .need_audio = ctx->need_audio,
            };
            std::string payload = starfish_json_build_load(&params);
            ctx->state = pipeline_state::LOADING;
            ctx->ended = false;
            ctx->eos_sent = false;
            ctx->eos_pending = false;
            ctx->ready_frames.clear();

            mp_info(ctx->log, "Starting Starfish load: video=%s audio=%s size=%dx%d fps=%d/%d window=%s\n",
                    params.video_codec ? params.video_codec : "(none)",
                    params.audio_codec ? params.audio_codec : "(none)",
                    params.width, params.height, params.fps_num, params.fps_den,
                    params.window_id ? params.window_id : "(acb)");
            mp_verbose(ctx->log, "Starfish Load payload: %s\n", payload.c_str());

            lock.unlock();
            if (!ensure_media(ctx)) {
                lock.lock();
                set_failed_locked(ctx, -1, "StarfishMediaAPIs allocation failed");
                wake_all(ctx);
                continue;
            }
            if (!ctx->media->notifyForeground())
                mp_warn(ctx->log, "Starfish notifyForeground failed\n");
            bool ok = ctx->media->Load(payload.c_str(), &player_callback, ctx);
            mp_info(ctx->log, "Starfish Load returned: %s\n", ok ? "success" : "failure");
            lock.lock();
            if (!ok) {
                set_failed_locked(ctx, -1, "Starfish Load failed");
                wake_all(ctx);
            }
            continue;
        }

        if (ctx->state == pipeline_state::FAILED) {
            ctx->cv.wait(lock, [&] { return ctx->stop || ctx->flush_requested ||
                                            ctx->state != pipeline_state::FAILED; });
            continue;
        }

        if (is_loaded_state(ctx->state)) {
            if (!ctx->play_requested && ctx->state == pipeline_state::PLAYING) {
                lock.unlock();
                if (!ctx->media->Pause())
                    mp_warn(ctx->log, "Starfish Pause failed\n");
                lock.lock();
                continue;
            }

            if (ctx->play_requested &&
                (ctx->state == pipeline_state::PAUSED || ctx->state == pipeline_state::LOADED)) {
                lock.unlock();
                if (!ctx->media->Play())
                    mp_warn(ctx->log, "Starfish Play failed\n");
                lock.lock();
                continue;
            }

            if (!ctx->video_queue.empty()) {
                queued_packet packet = ctx->video_queue.front();
                lock.unlock();
                bool buffer_full = false;
                bool ok = try_feed_packet(ctx, STARFISH_STREAM_VIDEO, packet, &buffer_full);
                lock.lock();
                if (ok) {
                    ctx->video_queue_bytes -= packet.data->size();
                    ctx->video_inflight_bytes += packet.data->size();
                    ctx->video_inflight.push_back(packet);
                    while (ctx->video_inflight_bytes > VIDEO_INFLIGHT_LIMIT &&
                           !ctx->video_inflight.empty()) {
                        ctx->video_inflight_bytes -= ctx->video_inflight.front().data->size();
                        ctx->video_inflight.pop_front();
                    }
                    ctx->video_queue.pop_front();
                    starfish_wakeup_cb cb = ctx->video_wakeup;
                    void *opaque = ctx->video_wakeup_opaque;
                    lock.unlock();
                    if (cb)
                        cb(opaque);
                    lock.lock();
                    continue;
                }
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(buffer_full ? 100 : 50));
                lock.lock();
                continue;
            }

            if (!ctx->audio_queue.empty()) {
                queued_packet packet = ctx->audio_queue.front();
                lock.unlock();
                bool buffer_full = false;
                bool ok = try_feed_packet(ctx, STARFISH_STREAM_AUDIO, packet, &buffer_full);
                lock.lock();
                if (ok) {
                    ctx->audio_queue_bytes -= packet.data->size();
                    ctx->audio_inflight_bytes += packet.data->size();
                    ctx->audio_inflight.push_back(packet);
                    while (ctx->audio_inflight_bytes > AUDIO_INFLIGHT_LIMIT &&
                           !ctx->audio_inflight.empty()) {
                        ctx->audio_inflight_bytes -= ctx->audio_inflight.front().data->size();
                        ctx->audio_inflight.pop_front();
                    }
                    ctx->audio_queue.pop_front();
                    starfish_wakeup_cb cb = ctx->audio_wakeup;
                    void *opaque = ctx->audio_wakeup_opaque;
                    lock.unlock();
                    if (cb)
                        cb(opaque);
                    lock.lock();
                    continue;
                }
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(buffer_full ? 100 : 50));
                lock.lock();
                continue;
            }

            if (ctx->eos_pending && !ctx->eos_sent) {
                ctx->eos_sent = true;
                lock.unlock();
                if (!ctx->media->pushEOS())
                    mp_warn(ctx->log, "Starfish pushEOS failed\n");
                lock.lock();
                ctx->eos_pending = false;
                continue;
            }
        }

        ctx->cv.wait(lock, [&] {
            return ctx->stop || ctx->flush_requested || should_start_load_locked(ctx) ||
                   (is_loaded_state(ctx->state) &&
                    ((!ctx->video_queue.empty()) || (!ctx->audio_queue.empty()) ||
                     (ctx->eos_pending && !ctx->eos_sent))) ||
                   (ctx->play_requested && (ctx->state == pipeline_state::PAUSED ||
                                            ctx->state == pipeline_state::LOADED)) ||
                   (!ctx->play_requested && ctx->state == pipeline_state::PLAYING);
        });
    }
}

static void player_callback(int32_t type, int64_t numValue, const char *strValue, void *opaque)
{
    struct starfish_ctx *ctx = static_cast<struct starfish_ctx *>(opaque);
    bool wake_video = false;
    bool wake_audio = false;
    bool call_play = false;

    mp_info(ctx->log, "Starfish callback type=%d(%s) num=%" PRId64 " str=%s\n",
            type, event_name(type), numValue, strValue ? strValue : "");

    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        switch (type) {
        case PF_EVENT_TYPE_FRAMEREADY: {
            struct starfish_video_frame frame = {
                .pts = numValue / 1000000000.0,
                .dts = numValue / 1000000000.0,
                .duration = ctx->fps > 0 ? 1.0 / ctx->fps : 0.0,
            };
            ctx->ready_frames.push_back(frame);
            wake_video = true;
            break;
        }
        case PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED:
            ctx->state = pipeline_state::LOADED;
            ctx->need_segment = true;
            ctx->ended = false;
            if (ctx->acb_id) {
                AcbAPI_setSinkType(ctx->acb_id, SINK_TYPE_MAIN);
                AcbAPI_setMediaId(ctx->acb_id, ctx->media->getMediaID());
                AcbAPI_setState(ctx->acb_id, APPSTATE_FOREGROUND, PLAYSTATE_LOADED,
                                &ctx->acb_task_id);
            }
            call_play = ctx->play_requested;
            wake_video = true;
            wake_audio = true;
            break;
        case PF_EVENT_TYPE_STR_STATE_UPDATE__PLAYING:
            ctx->state = pipeline_state::PLAYING;
            if (ctx->acb_id) {
                AcbAPI_setState(ctx->acb_id, APPSTATE_FOREGROUND, PLAYSTATE_PLAYING,
                                &ctx->acb_task_id);
            }
            wake_video = true;
            wake_audio = true;
            break;
        case PF_EVENT_TYPE_STR_STATE_UPDATE__PAUSED:
            ctx->state = pipeline_state::PAUSED;
            if (ctx->acb_id) {
                AcbAPI_setState(ctx->acb_id, APPSTATE_FOREGROUND, PLAYSTATE_PAUSED,
                                &ctx->acb_task_id);
            }
            wake_video = true;
            wake_audio = true;
            break;
        case PF_EVENT_TYPE_STR_STATE_UPDATE__ENDOFSTREAM:
            ctx->ended = true;
            wake_video = true;
            wake_audio = true;
            break;
        case PF_EVENT_TYPE_STR_STATE_UPDATE__UNLOADCOMPLETED:
            ctx->state = pipeline_state::IDLE;
            ctx->ended = true;
            ctx->eos_sent = false;
            ctx->eos_pending = false;
            ctx->need_segment = false;
            ctx->ready_frames.clear();
            ctx->video_inflight.clear();
            ctx->audio_inflight.clear();
            ctx->video_inflight_bytes = 0;
            ctx->audio_inflight_bytes = 0;
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
            break;
        case PF_EVENT_TYPE_STR_VIDEO_INFO:
            if (ctx->acb_id && strValue)
                AcbAPI_setMediaVideoData(ctx->acb_id, strValue);
            break;
        case PF_EVENT_TYPE_INT_ERROR:
        case PF_EVENT_TYPE_STR_ERROR:
            mp_err(ctx->log, "Starfish callback error type=%d num=%" PRId64 " str=%s\n",
                   type, numValue, strValue ? strValue : "");
            if (ctx->state == pipeline_state::LOADING)
                set_failed_locked(ctx, (int)numValue, strValue);
            wake_video = true;
            wake_audio = true;
            break;
        default:
            break;
        }
    }

    if (call_play && !ctx->media->Play())
        mp_err(ctx->log, "Starfish Play failed after load\n");
    ctx->cv.notify_all();
    if (wake_video)
        wake_stream(ctx, STARFISH_STREAM_VIDEO);
    if (wake_audio)
        wake_stream(ctx, STARFISH_STREAM_AUDIO);
}

extern "C" {

struct starfish_ctx *starfish_ctx_create(struct mp_log *log)
{
    struct starfish_ctx *ctx = new starfish_ctx();
    ctx->log = mp_log_new(nullptr, log, "starfish");
    ctx->worker = std::thread(worker_loop, ctx);
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

    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        ctx->stop = true;
    }
    ctx->cv.notify_all();
    if (ctx->worker.joinable())
        ctx->worker.join();

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
    return hwctx ? (struct starfish_ctx *)hwctx->conversion_config : nullptr;
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
    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        if (ctx->state == pipeline_state::LOADING || is_loaded_state(ctx->state))
            return false;
        ctx->window_id = window_id ? window_id : "";
        if (ctx->state == pipeline_state::WAIT_WINDOW && !ctx->window_id.empty())
            ctx->state = pipeline_state::IDLE;
    }
    ctx->cv.notify_all();
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
    if (ctx->state == pipeline_state::LOADING || is_loaded_state(ctx->state))
        return false;
    ctx->width = width;
    ctx->height = height;
    if (fps > 0) {
        AVRational r = av_d2q(fps, 1000000);
        ctx->fps = av_q2d(r);
        ctx->fps_num = r.num;
        ctx->fps_den = r.den;
    }
    update_adaptive_caps(ctx);
    return true;
}

bool starfish_ctx_set_display_window(struct starfish_ctx *ctx,
                                     int src_x, int src_y, int src_w, int src_h,
                                     int dst_x, int dst_y, int dst_w, int dst_h)
{
    std::lock_guard<std::mutex> lock(ctx->lock);
    if (!ctx->window_id.empty())
        return true;
    if (!ensure_acb(ctx))
        return false;

    return AcbAPI_setCustomDisplayWindow(ctx->acb_id,
                                         src_x, src_y, src_w, src_h,
                                         dst_x, dst_y, dst_w, dst_h,
                                         false, &ctx->acb_task_id);
}

bool starfish_ctx_configure_video(struct starfish_ctx *ctx,
                                  const struct mp_codec_params *codec)
{
    const char *name = video_codec_name((enum AVCodecID)mp_codec_to_av_codec_id(codec->codec));
    if (!name)
        return false;

    std::lock_guard<std::mutex> lock(ctx->lock);
    if (ctx->state == pipeline_state::LOADING || is_loaded_state(ctx->state))
        return false;

    ctx->video_codec = name;
    if (codec->lav_codecpar) {
        ctx->width = codec->lav_codecpar->width;
        ctx->height = codec->lav_codecpar->height;
    }
    if (codec->fps > 0.0) {
        AVRational r = av_d2q(codec->fps, 1000000);
        ctx->fps = av_q2d(r);
        ctx->fps_num = r.num;
        ctx->fps_den = r.den;
    }
    update_adaptive_caps(ctx);
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
    if (ctx->state == pipeline_state::LOADING || is_loaded_state(ctx->state))
        return false;
    ctx->audio_codec = name;
    ctx->need_audio = true;
    return true;
}

int starfish_ctx_feed_video(struct starfish_ctx *ctx, const void *data, size_t size,
                            double pts)
{
    std::lock_guard<std::mutex> lock(ctx->lock);
    if (ctx->state == pipeline_state::FAILED)
        return STARFISH_FEED_ERROR;
    if (!have_load_config_locked(ctx))
        return STARFISH_FEED_ERROR;
    if (ctx->video_queue_bytes + size > VIDEO_QUEUE_LIMIT)
        return STARFISH_FEED_AGAIN;

    queued_packet packet;
    packet.data = std::make_shared<std::vector<uint8_t>>((const uint8_t *)data,
                                                         (const uint8_t *)data + size);
    packet.pts_ns = pts == MP_NOPTS_VALUE ? 0 : (int64_t)(pts * 1e9);
    ctx->video_queue_bytes += size;
    ctx->video_queue.push_back(std::move(packet));
    if (ctx->window_id.empty() && !ctx->acb_id)
        ctx->state = pipeline_state::WAIT_WINDOW;
    ctx->cv.notify_all();
    return STARFISH_FEED_OK;
}

int starfish_ctx_feed_audio(struct starfish_ctx *ctx, const void *data, size_t size,
                            int64_t pts_ns)
{
    std::lock_guard<std::mutex> lock(ctx->lock);
    if (ctx->state == pipeline_state::FAILED)
        return STARFISH_FEED_ERROR;
    if (ctx->audio_queue_bytes + size > AUDIO_QUEUE_LIMIT)
        return STARFISH_FEED_AGAIN;

    queued_packet packet;
    packet.data = std::make_shared<std::vector<uint8_t>>((const uint8_t *)data,
                                                         (const uint8_t *)data + size);
    packet.pts_ns = pts_ns;
    ctx->audio_queue_bytes += size;
    ctx->audio_queue.push_back(std::move(packet));
    ctx->cv.notify_all();
    return STARFISH_FEED_OK;
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
    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        ctx->play_requested = true;
    }
    ctx->cv.notify_all();
    return true;
}

bool starfish_ctx_pause(struct starfish_ctx *ctx)
{
    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        ctx->play_requested = false;
    }
    ctx->cv.notify_all();
    return true;
}

bool starfish_ctx_flush(struct starfish_ctx *ctx, double pts)
{
    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        ctx->flush_requested = true;
        ctx->flush_pts_ns = pts == MP_NOPTS_VALUE ? 0 : (int64_t)(pts * 1e9);
    }
    ctx->cv.notify_all();
    return true;
}

bool starfish_ctx_push_eos(struct starfish_ctx *ctx)
{
    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        ctx->eos_pending = true;
        ctx->ended = false;
    }
    ctx->cv.notify_all();
    return true;
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

} // extern "C"
