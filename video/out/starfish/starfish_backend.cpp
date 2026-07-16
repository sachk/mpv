/*
 * Narrow C-style wrapper around the LG webOS Starfish SDK. The C++ headers
 * and exception handling are confined to this translation unit so the rest
 * of the mpv tree links against a stable C ABI.
 */

#include "starfish_backend.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <inttypes.h>
#include <memory>
#include <mutex>
#include <string>

#include <appswitching-control-block/AcbAPI.h>
#include <player-factory/custompipeline.hpp>
#include <player-factory/customplayer.hpp>
#include <starfish-media-pipeline/StarfishMediaAPIs.h>

extern "C" {
#include "common/msg.h"
#include "osdep/timer.h"
}

namespace {

std::mutex g_prime_lock;
std::unique_ptr<StarfishMediaAPIs> g_primed_media;

constexpr int64_t kSdkSlowCallNs = 50LL * 1000 * 1000;

bool sdk_verbose_call_logging()
{
    const char *value = getenv("STARFISH_SDK_CALL_LOG");
    return value && value[0] && strcmp(value, "0") != 0;
}

bool sdk_call_is_chatty(const char *what)
{
    return what && (strcmp(what, "Feed") == 0 ||
                    strcmp(what, "getCurrentPlaytime") == 0);
}

bool sdk_call_use_trace(const char *what)
{
    return sdk_call_is_chatty(what) && !sdk_verbose_call_logging();
}

void log_sdk_call_begin(struct mp_log *log, const char *what)
{
    if (sdk_call_use_trace(what))
        mp_trace(log, "Starfish SDK begin %s\n", what ? what : "(unknown)");
    else
        mp_info(log, "Starfish SDK begin %s\n", what ? what : "(unknown)");
}

void log_sdk_call_end(struct mp_log *log, const char *what, int64_t start_ns,
                      bool ok)
{
    const int64_t elapsed_ns = mp_time_ns() - start_ns;
    if (sdk_call_use_trace(what)) {
        mp_trace(log, "Starfish SDK end %s ok=%d duration=%.1fms\n",
                 what ? what : "(unknown)", ok, elapsed_ns / 1e6);
    } else {
        mp_info(log, "Starfish SDK end %s ok=%d duration=%.1fms\n",
                what ? what : "(unknown)", ok, elapsed_ns / 1e6);
    }
    if (elapsed_ns > kSdkSlowCallNs) {
        mp_warn(log, "Starfish SDK slow %s ok=%d duration=%.1fms\n",
                what ? what : "(unknown)", ok, elapsed_ns / 1e6);
    }
}

template <typename F>
bool try_bool(struct mp_log *log, const char *what, F &&fn)
{
    const int64_t start_ns = mp_time_ns();
    log_sdk_call_begin(log, what);
    bool ok = false;
    try {
        ok = fn();
    } catch (const std::exception &e) {
        mp_err(log, "Starfish %s threw: %s\n", what, e.what());
    } catch (...) {
        mp_err(log, "Starfish %s threw unknown exception\n", what);
    }
    log_sdk_call_end(log, what, start_ns, ok);
    return ok;
}

template <typename F>
std::string try_string(struct mp_log *log, const char *what, F &&fn)
{
    const int64_t start_ns = mp_time_ns();
    log_sdk_call_begin(log, what);
    std::string result;
    try {
        result = fn();
    } catch (const std::exception &e) {
        mp_err(log, "Starfish %s threw: %s\n", what, e.what());
    } catch (...) {
        mp_err(log, "Starfish %s threw unknown exception\n", what);
    }
    log_sdk_call_end(log, what, start_ns, !result.empty());
    return result;
}

std::unique_ptr<StarfishMediaAPIs> make_media(struct mp_log *log)
{
    // StarfishMediaAPIs' constructor creates a ResourceManagerClient that does
    // an anonymous LS2 registration (LSRegisterPubPriv). When the app is
    // relaunched quickly -- or a prior playback's pipeline is torn down right
    // before a new one starts -- the previous registration may not be reaped by
    // the LS2 hub yet, so the constructor throws "LSRegisterPubPriv FAILED".
    // With no retry that turned into a permanent black screen for the launch.
    // The deregistration race clears in well under a second once the old
    // client's socket is reaped, so retry a few times with a short backoff
    // before giving up.
    constexpr int kMaxAttempts = 6;
    constexpr int64_t kBackoffNs = 150LL * 1000 * 1000; // 150 ms
    for (int attempt = 1; attempt <= kMaxAttempts; attempt++) {
        try {
            return std::make_unique<StarfishMediaAPIs>();
        } catch (const std::exception &e) {
            if (log)
                mp_err(log,
                       "StarfishMediaAPIs allocation threw (attempt %d/%d): %s\n",
                       attempt, kMaxAttempts, e.what());
        } catch (...) {
            if (log)
                mp_err(log,
                       "StarfishMediaAPIs allocation threw unknown exception "
                       "(attempt %d/%d)\n",
                       attempt, kMaxAttempts);
        }
        if (attempt < kMaxAttempts)
            mp_sleep_ns(kBackoffNs);
    }
    return nullptr;
}

sf_backend_event_type map_event(int32_t type)
{
    switch (type) {
    case PF_EVENT_TYPE_FRAMEREADY:
        return SF_EVENT_FRAME_READY;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__PRELOADCOMPLETED:
        return SF_EVENT_PRELOAD_COMPLETED;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED:
        return SF_EVENT_LOAD_COMPLETED;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__UNLOADCOMPLETED:
        return SF_EVENT_UNLOAD_COMPLETED;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__PLAYING:
        return SF_EVENT_PLAYING;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__PAUSED:
        return SF_EVENT_PAUSED;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__SEEKDONE:
        return SF_EVENT_SEEK_DONE;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__ENDOFSTREAM:
        return SF_EVENT_END_OF_STREAM;
    case PF_EVENT_TYPE_INT_BUFFERLOW:
    case PF_EVENT_TYPE_STR_BUFFERLOW:
        return SF_EVENT_BUFFER_LOW;
    case PF_EVENT_TYPE_STR_BUFFERFULL:
        return SF_EVENT_BUFFER_FULL;
    case PF_EVENT_TYPE_STR_VIDEO_INFO:
        return SF_EVENT_VIDEO_INFO;
    case PF_EVENT_TYPE_STR_AUDIO_INFO:
        return SF_EVENT_AUDIO_INFO;
    case PF_EVENT_TYPE_STR_ERROR:
    case PF_EVENT_TYPE_INT_ERROR:
        return SF_EVENT_ERROR;
    default:
        return SF_EVENT_UNKNOWN;
    }
}

void acb_callback_noop(long, long, long, long, long, const char *) {}

} // namespace

struct sf_backend {
    struct mp_log *log = nullptr;
    std::mutex callback_lock;
    sf_backend_callbacks callbacks = {};
    std::unique_ptr<StarfishMediaAPIs> media;
    std::string cached_media_id;
};

struct sf_backend_acb {
    struct mp_log *log = nullptr;
    long acb_id = 0;
    long task_id = 0;
    bool initialized = false;
};

static void player_callback_trampoline(int32_t type, int64_t num_value,
                                       const char *str_value, void *opaque)
{
    auto *b = static_cast<sf_backend *>(opaque);
    if (!b)
        return;
    std::lock_guard<std::mutex> lock(b->callback_lock);
    if (!b->callbacks.event)
        return;
    b->callbacks.event(b->callbacks.opaque, map_event(type), num_value,
                       str_value);
}

struct sf_backend *sf_backend_create(struct mp_log *log,
                                     const struct sf_backend_callbacks *callbacks)
{
    auto *b = new sf_backend();
    b->log = log;
    if (callbacks)
        b->callbacks = *callbacks;
    {
        std::lock_guard<std::mutex> lk(g_prime_lock);
        if (g_primed_media)
            b->media = std::move(g_primed_media);
    }
    if (!b->media)
        b->media = make_media(log);
    if (!b->media) {
        delete b;
        return nullptr;
    }
    return b;
}

void sf_backend_destroy(struct sf_backend *b)
{
    if (!b)
        return;
    {
        // Waiting for this lock fences any callback already executing against
        // the owning starfish_ctx. Nulling the callback before Unload also
        // makes teardown-generated events harmless.
        std::lock_guard<std::mutex> lock(b->callback_lock);
        b->callbacks = {};
    }
    if (b->media)
        try_bool(b->log, "Unload",
                 [&] { return b->media->Unload(); });
    delete b;
}

bool sf_backend_notify_foreground(struct sf_backend *b)
{
    if (!b || !b->media)
        return false;
    return try_bool(b->log, "notifyForeground",
                    [&] { return b->media->notifyForeground(); });
}

bool sf_backend_load(struct sf_backend *b,
                     const struct sf_backend_load_params *params)
{
    if (!b || !b->media || !params || !params->payload_json)
        return false;
    mp_info(b->log, "Starfish Load payload: %s\n", params->payload_json);
    bool ok = try_bool(b->log, "Load", [&] {
        return b->media->Load(params->payload_json,
                              &player_callback_trampoline, b);
    });
    if (ok)
        b->cached_media_id.clear();
    return ok;
}

bool sf_backend_unload(struct sf_backend *b)
{
    if (!b || !b->media)
        return true;
    bool ok = try_bool(b->log, "Unload",
                       [&] { return b->media->Unload(); });
    b->cached_media_id.clear();
    return ok;
}

bool sf_backend_play(struct sf_backend *b)
{
    if (!b || !b->media)
        return false;
    return try_bool(b->log, "Play", [&] { return b->media->Play(); });
}

bool sf_backend_pause(struct sf_backend *b)
{
    if (!b || !b->media)
        return false;
    return try_bool(b->log, "Pause", [&] { return b->media->Pause(); });
}

bool sf_backend_flush(struct sf_backend *b)
{
    if (!b || !b->media)
        return false;
    return try_bool(b->log, "flush", [&] { return b->media->flush(); });
}

bool sf_backend_push_eos(struct sf_backend *b)
{
    if (!b || !b->media)
        return false;
    return try_bool(b->log, "pushEOS",
                    [&] { return b->media->pushEOS(); });
}

bool sf_backend_set_time_to_decode(struct sf_backend *b, int64_t pts_ns)
{
    if (!b || !b->media)
        return false;
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"position\":%" PRId64 "}", pts_ns);
    if (try_bool(b->log, "setTimeToDecode",
                 [&] { return b->media->setTimeToDecode(payload); }))
        return true;

    auto *player =
        static_cast<mediapipeline::CustomPlayer *>(b->media->player.get());
    auto *pipeline = player ? static_cast<mediapipeline::CustomPipeline *>(
                                  player->getPipeline().get())
                            : nullptr;
    if (!pipeline)
        return false;

    const int64_t fallback_start_ns = mp_time_ns();
    log_sdk_call_begin(b->log, "setTimeToDecodeFallback");
    bool fallback_ok = false;
    try {
        MEDIA_CUSTOM_CONTENT_INFO_T info;
        pipeline->loadSpi_getInfo(&info);
        info.ptsToDecode = pts_ns;
        pipeline->setContentInfo(MEDIA_CUSTOM_SRC_TYPE_ES, &info);
        fallback_ok = true;
    } catch (...) {
        mp_warn(b->log, "Starfish setTimeToDecode fallback failed\n");
    }
    log_sdk_call_end(b->log, "setTimeToDecodeFallback", fallback_start_ns,
                     fallback_ok);
    return fallback_ok;
}

bool sf_backend_send_segment_event(struct sf_backend *b)
{
    if (!b || !b->media)
        return false;
    auto *player =
        static_cast<mediapipeline::CustomPlayer *>(b->media->player.get());
    auto *pipeline = player ? static_cast<mediapipeline::CustomPipeline *>(
                                  player->getPipeline().get())
                            : nullptr;
    if (!pipeline)
        return true;
    try {
        pipeline->sendSegmentEvent();
        return true;
    } catch (...) {
        mp_warn(b->log, "Starfish sendSegmentEvent threw\n");
        return false;
    }
}

bool sf_backend_set_play_rate(struct sf_backend *b, int play_rate_millis,
                              bool audio_output)
{
    if (!b || !b->media)
        return false;
    char payload[64];
    snprintf(payload, sizeof(payload),
             "{\"audioOutput\":%s,\"playRate\":%.3f}",
             audio_output ? "true" : "false", play_rate_millis / 1000.0);
    return try_bool(b->log, "SetPlayRate",
                    [&] { return b->media->SetPlayRate(payload); });
}

bool sf_backend_set_hdr_info(struct sf_backend *b, const char *payload_json)
{
    if (!b || !b->media || !payload_json)
        return false;
    return try_bool(b->log, "setHdrInfo",
                    [&] { return b->media->setHdrInfo(payload_json); });
}

enum sf_backend_feed_result sf_backend_feed(struct sf_backend *b,
                                            const struct sf_backend_packet *packet)
{
    if (!b || !b->media || !packet || !packet->data || packet->size == 0)
        return SF_BACKEND_FEED_ERROR;

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"bufferAddr\":\"0x%" PRIxPTR "\",\"bufferSize\":%zu,"
             "\"pts\":%" PRId64 ",\"esData\":%d}",
             reinterpret_cast<uintptr_t>(packet->data), packet->size,
             packet->pts_ns,
             packet->stream == SF_BACKEND_AUDIO ? 2 : 1);

    std::string result = try_string(b->log, "Feed", [&] {
        return b->media->Feed(payload);
    });

    if (result.find("Ok") != std::string::npos)
        return SF_BACKEND_FEED_OK;
    if (result.find("BufferFull") != std::string::npos)
        return SF_BACKEND_FEED_BUFFER_FULL;
    if (result.find("Error") != std::string::npos)
        return SF_BACKEND_FEED_ERROR;
    return SF_BACKEND_FEED_RETRY;
}

bool sf_backend_get_current_playtime(struct sf_backend *b,
                                     struct sf_backend_clock_sample *sample)
{
    if (!sample)
        return false;
    *sample = {};
    if (!b || !b->media)
        return false;

    const int64_t before = mp_time_ns();
    log_sdk_call_begin(b->log, "getCurrentPlaytime");
    int64_t playtime_ns = -1;
    bool ok = false;
    try {
        playtime_ns = b->media->getCurrentPlaytime();
        ok = playtime_ns >= 0;
    } catch (const std::exception &e) {
        mp_warn(b->log, "Starfish getCurrentPlaytime threw: %s\n", e.what());
        log_sdk_call_end(b->log, "getCurrentPlaytime", before, false);
        return false;
    } catch (...) {
        mp_warn(b->log, "Starfish getCurrentPlaytime threw unknown exception\n");
        log_sdk_call_end(b->log, "getCurrentPlaytime", before, false);
        return false;
    }
    const int64_t after = mp_time_ns();
    log_sdk_call_end(b->log, "getCurrentPlaytime", before, ok);
    if (playtime_ns < 0)
        return false;

    sample->valid = true;
    // StarfishMediaAPIs returns nanoseconds, matching packet/feed timestamps.
    sample->pts = playtime_ns / 1e9;
    sample->host_time_ns = before + (after - before) / 2;
    sample->query_duration_ns = after - before;
    return true;
}

const char *sf_backend_get_media_id(struct sf_backend *b)
{
    if (!b || !b->media)
        return nullptr;
    if (b->cached_media_id.empty()) {
        try {
            b->cached_media_id = b->media->getMediaID();
        } catch (...) {
            return nullptr;
        }
    }
    return b->cached_media_id.empty() ? nullptr : b->cached_media_id.c_str();
}

bool sf_backend_prime_media(struct mp_log *log)
{
    std::lock_guard<std::mutex> lk(g_prime_lock);
    if (g_primed_media)
        return true;
    g_primed_media = make_media(log);
    return !!g_primed_media;
}

struct sf_backend_acb *sf_backend_acb_create(struct mp_log *log,
                                             const char *app_id)
{
    auto *acb = new sf_backend_acb();
    acb->log = log;
    acb->acb_id = AcbAPI_create();
    if (!acb->acb_id) {
        mp_err(log, "AcbAPI_create failed\n");
        delete acb;
        return nullptr;
    }
    if (!AcbAPI_initialize(acb->acb_id, PLAYER_TYPE_MSE,
                           app_id ? app_id : "mpv", &acb_callback_noop)) {
        mp_err(log, "AcbAPI_initialize failed\n");
        AcbAPI_destroy(acb->acb_id);
        delete acb;
        return nullptr;
    }
    acb->initialized = true;
    return acb;
}

void sf_backend_acb_destroy(struct sf_backend_acb *acb)
{
    if (!acb)
        return;
    if (acb->initialized)
        AcbAPI_finalize(acb->acb_id);
    if (acb->acb_id)
        AcbAPI_destroy(acb->acb_id);
    delete acb;
}

bool sf_backend_acb_set_display_window(struct sf_backend_acb *acb,
                                       int src_x, int src_y, int src_w, int src_h,
                                       int dst_x, int dst_y, int dst_w, int dst_h)
{
    if (!acb)
        return false;
    int disp = AcbAPI_setDisplayWindow(acb->acb_id, dst_x, dst_y, dst_w, dst_h,
                                       false, &acb->task_id);
    int custom = AcbAPI_setCustomDisplayWindow(acb->acb_id, src_x, src_y,
                                               src_w, src_h, dst_x, dst_y,
                                               dst_w, dst_h, false,
                                               &acb->task_id);
    mp_info(acb->log,
            "Starfish ACB window src=%d,%d %dx%d dst=%d,%d %dx%d "
            "ret display=%d custom=%d\n",
            src_x, src_y, src_w, src_h, dst_x, dst_y, dst_w, dst_h, disp,
            custom);
    return disp == 0 || custom == 0 || disp == 1 || custom == 1;
}

void sf_backend_acb_attach(struct sf_backend_acb *acb, const char *media_id)
{
    if (!acb)
        return;
    AcbAPI_setSinkType(acb->acb_id, SINK_TYPE_MAIN);
    if (media_id && media_id[0])
        AcbAPI_setMediaId(acb->acb_id, media_id);
    AcbAPI_setState(acb->acb_id, APPSTATE_FOREGROUND, PLAYSTATE_LOADED,
                    &acb->task_id);
}

void sf_backend_acb_set_play_state(struct sf_backend_acb *acb, int play_state)
{
    if (!acb)
        return;
    AcbAPI_PlayState state = PLAYSTATE_LOADED;
    switch (play_state) {
    case SF_ACB_LOADED:   state = PLAYSTATE_LOADED;   break;
    case SF_ACB_PLAYING:  state = PLAYSTATE_PLAYING;  break;
    case SF_ACB_PAUSED:   state = PLAYSTATE_PAUSED;   break;
    case SF_ACB_UNLOADED: state = PLAYSTATE_UNLOADED; break;
    }
    AcbAPI_setState(acb->acb_id, APPSTATE_FOREGROUND, state, &acb->task_id);
}

void sf_backend_acb_set_video_info(struct sf_backend_acb *acb,
                                   const char *payload)
{
    if (!acb || !payload)
        return;
    AcbAPI_setMediaVideoData(acb->acb_id, payload);
}
