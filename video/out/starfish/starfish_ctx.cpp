#include "starfish_ctx.h"

#include <exception>
#include <inttypes.h>
#include <atomic>
#include <chrono>
#include <cmath>
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
#include <libavutil/dovi_meta.h>
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

enum class dovi_policy {
    AUTO,
    PASSTHROUGH,
    P7_FALLBACK,
    HDR10,
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

bool env_wants_audio_hint()
{
    const char *hint = getenv("STARFISH_AUDIO_HINT");
    return hint && hint[0] && strcmp(hint, "0") != 0;
}

dovi_policy get_dovi_policy()
{
    const char *value = getenv("STARFISH_DOVI_POLICY");
    if (!value || !value[0] || strcmp(value, "auto") == 0)
        return dovi_policy::AUTO;
    if (strcmp(value, "passthrough") == 0)
        return dovi_policy::PASSTHROUGH;
    if (strcmp(value, "p7-fallback") == 0)
        return dovi_policy::P7_FALLBACK;
    if (strcmp(value, "hdr10") == 0)
        return dovi_policy::HDR10;
    return dovi_policy::AUTO;
}

const char *dovi_policy_name(dovi_policy policy)
{
    switch (policy) {
    case dovi_policy::AUTO:
        return "auto";
    case dovi_policy::PASSTHROUGH:
        return "passthrough";
    case dovi_policy::P7_FALLBACK:
        return "p7-fallback";
    case dovi_policy::HDR10:
        return "hdr10";
    }
    return "auto";
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
    unsigned int video_codec_tag = 0;
    int audio_channels = 0;
    int audio_samplerate = 0;
    int audio_profile = 0;
    bool audio_raw = false;
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
    dovi_policy dovi_mode = dovi_policy::AUTO;
    bool source_dovi = false;
    bool effective_dovi = false;
    uint8_t dv_profile = 0;
    uint8_t dv_level = 0;
    uint8_t dv_bl_signal_compatibility_id = 0;
    bool dv_rpu_present = false;
    bool dv_el_present = false;
    bool dv_bl_present = false;
    struct pl_color_space video_color = pl_color_space_unknown;
    struct pl_color_repr video_repr = pl_color_repr_unknown;

    pipeline_state state = pipeline_state::IDLE;
    bool play_requested = true;
    bool need_segment = false;
    bool flush_requested = false;
    int64_t flush_pts_ns = 0;
    bool pending_seek_target = false;
    int64_t pending_seek_target_pts_ns = 0;
    bool have_segment_target = false;
    int64_t segment_target_pts_ns = 0;
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
    uint64_t flush_count = 0;
    uint64_t segment_restart_count = 0;
    uint64_t bufferfull_count = 0;
    uint64_t bufferlow_count = 0;
    uint64_t seekdone_count = 0;
};

static std::mutex g_current_lock;
static struct starfish_ctx *g_current_ctx;
static std::mutex g_media_init_lock;
static std::unique_ptr<StarfishMediaAPIs> g_primed_media;
static void player_callback(int32_t type, int64_t numValue, const char *strValue,
                            void *opaque);
static void worker_loop(struct starfish_ctx *ctx);

template <typename F>
static bool media_call_bool(struct starfish_ctx *ctx, const char *what, F &&fn)
{
    try {
        return fn();
    } catch (const std::exception &e) {
        mp_err(ctx->log, "Starfish %s threw exception: %s\n", what, e.what());
    } catch (...) {
        mp_err(ctx->log, "Starfish %s threw unknown exception\n", what);
    }
    return false;
}

template <typename F>
static std::string media_call_string(struct starfish_ctx *ctx, const char *what, F &&fn)
{
    try {
        return fn();
    } catch (const std::exception &e) {
        mp_err(ctx->log, "Starfish %s threw exception: %s\n", what, e.what());
    } catch (...) {
        mp_err(ctx->log, "Starfish %s threw unknown exception\n", what);
    }
    return "";
}

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
    case PF_EVENT_TYPE_STR_STATE_UPDATE__SEEKDONE:
        return "SEEKDONE";
    case PF_EVENT_TYPE_STR_STATE_UPDATE__ENDOFSTREAM:
        return "ENDOFSTREAM";
    case PF_EVENT_TYPE_INT_BUFFER_RANGE_INFO:
        return "BUFFER_RANGE_INFO";
    case PF_EVENT_TYPE_INT_BUFFERLOW:
        return "INT_BUFFERLOW";
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

static std::unique_ptr<StarfishMediaAPIs> create_media_instance(struct mp_log *log)
{
    try {
        return std::make_unique<StarfishMediaAPIs>();
    } catch (const std::exception &e) {
        if (log)
            mp_err(log, "StarfishMediaAPIs allocation threw exception: %s\n", e.what());
    } catch (...) {
        if (log)
            mp_err(log, "StarfishMediaAPIs allocation threw unknown exception\n");
    }
    return nullptr;
}

static bool ensure_media(struct starfish_ctx *ctx)
{
    if (ctx->media)
        return true;

    std::lock_guard<std::mutex> media_lock(g_media_init_lock);
    if (g_primed_media) {
        ctx->media = std::move(g_primed_media);
        return true;
    }
    ctx->media = create_media_instance(ctx->log);
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

static const AVDOVIDecoderConfigurationRecord *find_dovi_config(
    const struct mp_codec_params *codec)
{
    if (!codec || !codec->lav_codecpar)
        return nullptr;

    for (int n = 0; n < codec->lav_codecpar->nb_coded_side_data; n++) {
        const AVPacketSideData *side_data = &codec->lav_codecpar->coded_side_data[n];
        if (side_data->type == AV_PKT_DATA_DOVI_CONF &&
            side_data->size >= (int)sizeof(AVDOVIDecoderConfigurationRecord))
        {
            return reinterpret_cast<const AVDOVIDecoderConfigurationRecord *>(side_data->data);
        }
    }

    return nullptr;
}

static bool dovi_track_is_dual_layer(const struct starfish_ctx *ctx)
{
    return ctx->dv_el_present;
}

static bool resolve_effective_dovi_locked(struct starfish_ctx *ctx)
{
    if (!ctx->source_dovi)
        return false;

    switch (ctx->dovi_mode) {
    case dovi_policy::HDR10:
        return false;
    case dovi_policy::PASSTHROUGH:
        return true;
    case dovi_policy::AUTO:
    case dovi_policy::P7_FALLBACK:
        if (ctx->dv_profile == 7 && dovi_track_is_dual_layer(ctx)) {
            mp_warn(ctx->log,
                    "Dolby Vision profile 7 dual-layer stream needs conversion; "
                    "falling back to HDR10 in current fork\n");
            return false;
        }
        return true;
    }

    return false;
}

static const char *hdr_type_name_locked(const struct starfish_ctx *ctx)
{
    switch (ctx->video_color.transfer) {
    case PL_COLOR_TRC_PQ:
        return "hdr10";
    case PL_COLOR_TRC_HLG:
        return "hlg";
    default:
        return "none";
    }
}

static bool hdr_sei_available_locked(const struct starfish_ctx *ctx)
{
    const struct pl_hdr_metadata *hdr = &ctx->video_color.hdr;
    const struct pl_raw_primaries *prim = &hdr->prim;
    return hdr->min_luma > 0.0f || hdr->max_luma > 0.0f ||
           hdr->max_cll > 0.0f || hdr->max_fall > 0.0f ||
           prim->red.x > 0.0f || prim->red.y > 0.0f ||
           prim->green.x > 0.0f || prim->green.y > 0.0f ||
           prim->blue.x > 0.0f || prim->blue.y > 0.0f ||
           prim->white.x > 0.0f || prim->white.y > 0.0f;
}

static int scale_chromaticity(float value)
{
    return value > 0.0f ? (int)llrintf(value * 50000.0f) : 0;
}

static int scale_luminance(float value)
{
    return value > 0.0f ? (int)llrintf(value * 10000.0f) : 0;
}

static int scale_content_light(float value)
{
    return value > 0.0f ? (int)llrintf(value) : 0;
}

static enum AVColorSpace color_system_to_av(enum pl_color_system sys)
{
    switch (sys) {
    case PL_COLOR_SYSTEM_UNKNOWN:
        return AVCOL_SPC_UNSPECIFIED;
    case PL_COLOR_SYSTEM_BT_601:
        return AVCOL_SPC_SMPTE170M;
    case PL_COLOR_SYSTEM_BT_709:
        return AVCOL_SPC_BT709;
    case PL_COLOR_SYSTEM_SMPTE_240M:
        return AVCOL_SPC_SMPTE240M;
    case PL_COLOR_SYSTEM_BT_2020_NC:
        return AVCOL_SPC_BT2020_NCL;
    case PL_COLOR_SYSTEM_BT_2020_C:
        return AVCOL_SPC_BT2020_CL;
    case PL_COLOR_SYSTEM_BT_2100_PQ:
    case PL_COLOR_SYSTEM_BT_2100_HLG:
        return AVCOL_SPC_ICTCP;
    case PL_COLOR_SYSTEM_DOLBYVISION:
    case PL_COLOR_SYSTEM_XYZ:
        return AVCOL_SPC_UNSPECIFIED;
    case PL_COLOR_SYSTEM_YCGCO:
        return AVCOL_SPC_YCGCO;
    case PL_COLOR_SYSTEM_RGB:
        return AVCOL_SPC_RGB;
    case PL_COLOR_SYSTEM_COUNT:
        return AVCOL_SPC_NB;
    }

    return AVCOL_SPC_UNSPECIFIED;
}

static enum AVColorRange color_levels_to_av(enum pl_color_levels levels)
{
    switch (levels) {
    case PL_COLOR_LEVELS_UNKNOWN:
        return AVCOL_RANGE_UNSPECIFIED;
    case PL_COLOR_LEVELS_LIMITED:
        return AVCOL_RANGE_MPEG;
    case PL_COLOR_LEVELS_FULL:
        return AVCOL_RANGE_JPEG;
    case PL_COLOR_LEVELS_COUNT:
        return AVCOL_RANGE_NB;
    }

    return AVCOL_RANGE_UNSPECIFIED;
}

static enum AVColorPrimaries color_primaries_to_av(enum pl_color_primaries prim)
{
    switch (prim) {
    case PL_COLOR_PRIM_UNKNOWN:
        return AVCOL_PRI_UNSPECIFIED;
    case PL_COLOR_PRIM_BT_601_525:
        return AVCOL_PRI_SMPTE170M;
    case PL_COLOR_PRIM_BT_601_625:
        return AVCOL_PRI_BT470BG;
    case PL_COLOR_PRIM_BT_709:
        return AVCOL_PRI_BT709;
    case PL_COLOR_PRIM_BT_470M:
        return AVCOL_PRI_BT470M;
    case PL_COLOR_PRIM_EBU_3213:
        return AVCOL_PRI_JEDEC_P22;
    case PL_COLOR_PRIM_BT_2020:
        return AVCOL_PRI_BT2020;
    case PL_COLOR_PRIM_CIE_1931:
        return AVCOL_PRI_SMPTE428;
    case PL_COLOR_PRIM_DCI_P3:
        return AVCOL_PRI_SMPTE431;
    case PL_COLOR_PRIM_DISPLAY_P3:
        return AVCOL_PRI_SMPTE432;
    case PL_COLOR_PRIM_FILM_C:
        return AVCOL_PRI_FILM;
    case PL_COLOR_PRIM_APPLE:
    case PL_COLOR_PRIM_ADOBE:
    case PL_COLOR_PRIM_PRO_PHOTO:
    case PL_COLOR_PRIM_V_GAMUT:
    case PL_COLOR_PRIM_S_GAMUT:
    case PL_COLOR_PRIM_ACES_AP0:
    case PL_COLOR_PRIM_ACES_AP1:
        return AVCOL_PRI_UNSPECIFIED;
    case PL_COLOR_PRIM_COUNT:
        return AVCOL_PRI_NB;
    }

    return AVCOL_PRI_UNSPECIFIED;
}

static enum AVColorTransferCharacteristic color_transfer_to_av(enum pl_color_transfer trc)
{
    switch (trc) {
    case PL_COLOR_TRC_UNKNOWN:
        return AVCOL_TRC_UNSPECIFIED;
    case PL_COLOR_TRC_BT_1886:
        return AVCOL_TRC_BT709;
    case PL_COLOR_TRC_SRGB:
        return AVCOL_TRC_IEC61966_2_1;
    case PL_COLOR_TRC_LINEAR:
        return AVCOL_TRC_LINEAR;
    case PL_COLOR_TRC_GAMMA22:
        return AVCOL_TRC_GAMMA22;
    case PL_COLOR_TRC_GAMMA28:
        return AVCOL_TRC_GAMMA28;
    case PL_COLOR_TRC_ST428:
        return AVCOL_TRC_SMPTE428;
    case PL_COLOR_TRC_PQ:
        return AVCOL_TRC_SMPTE2084;
    case PL_COLOR_TRC_HLG:
        return AVCOL_TRC_ARIB_STD_B67;
    case PL_COLOR_TRC_GAMMA18:
    case PL_COLOR_TRC_GAMMA20:
    case PL_COLOR_TRC_GAMMA24:
    case PL_COLOR_TRC_GAMMA26:
    case PL_COLOR_TRC_PRO_PHOTO:
    case PL_COLOR_TRC_V_LOG:
    case PL_COLOR_TRC_S_LOG1:
    case PL_COLOR_TRC_S_LOG2:
        return AVCOL_TRC_UNSPECIFIED;
    case PL_COLOR_TRC_COUNT:
        return AVCOL_TRC_NB;
    }

    return AVCOL_TRC_UNSPECIFIED;
}

static bool apply_hdr_info(struct starfish_ctx *ctx)
{
    struct starfish_json_hdr_info_params params = {};

    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        params.hdr_type = hdr_type_name_locked(ctx);
        params.has_sei = hdr_sei_available_locked(ctx);
        if (strcmp(params.hdr_type, "none") == 0 || !params.has_sei)
            return false;

        const struct pl_hdr_metadata *hdr = &ctx->video_color.hdr;
        params.display_primaries_x0 = scale_chromaticity(hdr->prim.green.x);
        params.display_primaries_y0 = scale_chromaticity(hdr->prim.green.y);
        params.display_primaries_x1 = scale_chromaticity(hdr->prim.blue.x);
        params.display_primaries_y1 = scale_chromaticity(hdr->prim.blue.y);
        params.display_primaries_x2 = scale_chromaticity(hdr->prim.red.x);
        params.display_primaries_y2 = scale_chromaticity(hdr->prim.red.y);
        params.white_point_x = scale_chromaticity(hdr->prim.white.x);
        params.white_point_y = scale_chromaticity(hdr->prim.white.y);
        params.min_display_mastering_luminance = scale_luminance(hdr->min_luma);
        params.max_display_mastering_luminance = scale_luminance(hdr->max_luma);
        params.max_content_light_level = scale_content_light(hdr->max_cll);
        params.max_pic_average_light_level = scale_content_light(hdr->max_fall);
        params.transfer_characteristics = color_transfer_to_av(ctx->video_color.transfer);
        params.color_primaries = color_primaries_to_av(ctx->video_color.primaries);
        params.matrix_coeffs = color_system_to_av(ctx->video_repr.sys);
        params.video_full_range_flag =
            color_levels_to_av(ctx->video_repr.levels) == AVCOL_RANGE_JPEG;
    }

    std::string payload = starfish_json_build_hdr_info(&params);
    mp_info(ctx->log, "Starfish setHdrInfo payload: %s\n", payload.c_str());
    if (!media_call_bool(ctx, "setHdrInfo", [&] { return ctx->media->setHdrInfo(payload.c_str()); }))
        mp_warn(ctx->log, "Starfish setHdrInfo failed\n");
    return true;
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
    if (ctx->need_audio && ctx->audio_queue.empty())
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
    if (media_call_bool(ctx, "setTimeToDecode",
                        [&] { return ctx->media->setTimeToDecode(payload.c_str()); }))
        return true;

    auto *player = static_cast<mediapipeline::CustomPlayer *>(ctx->media->player.get());
    auto *pipeline = player ? static_cast<mediapipeline::CustomPipeline *>(player->getPipeline().get())
                            : nullptr;
    if (!pipeline)
        return false;

    MEDIA_CUSTOM_CONTENT_INFO_T content_info;
    try {
        pipeline->loadSpi_getInfo(&content_info);
    } catch (const std::exception &e) {
        mp_err(ctx->log, "Starfish loadSpi_getInfo threw exception: %s\n", e.what());
        return false;
    } catch (...) {
        mp_err(ctx->log, "Starfish loadSpi_getInfo threw unknown exception\n");
        return false;
    }
    content_info.ptsToDecode = pts_ns;
    try {
        pipeline->setContentInfo(MEDIA_CUSTOM_SRC_TYPE_ES, &content_info);
    } catch (const std::exception &e) {
        mp_err(ctx->log, "Starfish setContentInfo threw exception: %s\n", e.what());
        return false;
    } catch (...) {
        mp_err(ctx->log, "Starfish setContentInfo threw unknown exception\n");
        return false;
    }
    return true;
}

static bool try_feed_packet(struct starfish_ctx *ctx, enum starfish_stream_type stream,
                            const queued_packet &packet, bool *buffer_full)
{
    if (stream == STARFISH_STREAM_VIDEO && ctx->need_segment) {
        const int64_t segment_pts =
            ctx->have_segment_target ? ctx->segment_target_pts_ns : packet.pts_ns;
        mp_info(ctx->log,
                "Starfish segment restart #%" PRIu64 ": target=%" PRId64
                " packet_pts=%" PRId64 " source=%s\n",
                ctx->segment_restart_count + 1, segment_pts, packet.pts_ns,
                ctx->have_segment_target ? "seek-target" : "packet");
        if (!set_time_to_decode(ctx, segment_pts)) {
            mp_warn(ctx->log, "Starfish setTimeToDecode failed for segment target %" PRId64
                              "\n",
                    segment_pts);
        }
        auto *player = static_cast<mediapipeline::CustomPlayer *>(ctx->media->player.get());
        auto *pipeline =
            player ? static_cast<mediapipeline::CustomPipeline *>(player->getPipeline().get())
                   : nullptr;
        if (pipeline) {
            try {
                pipeline->sendSegmentEvent();
            } catch (const std::exception &e) {
                mp_err(ctx->log, "Starfish sendSegmentEvent threw exception: %s\n", e.what());
                *buffer_full = true;
                return false;
            } catch (...) {
                mp_err(ctx->log, "Starfish sendSegmentEvent threw unknown exception\n");
                *buffer_full = true;
                return false;
            }
        }
        ctx->segment_restart_count += 1;
        ctx->need_segment = false;
        ctx->have_segment_target = false;
        ctx->pending_seek_target = false;
    }

    std::string payload = starfish_json_build_feed(stream, packet.data->data(),
                                                   packet.data->size(), packet.pts_ns);
    std::string result = media_call_string(ctx, "Feed",
                                           [&] { return ctx->media->Feed(payload.c_str()); });
    mp_info(ctx->log, "Starfish %s feed: size=%zu pts=%" PRId64 " result=%s\n",
            stream == STARFISH_STREAM_VIDEO ? "video" : "audio",
            packet.data->size(), packet.pts_ns, result.c_str());

    if (result.find("Ok") != std::string::npos)
        return true;

    if (result.find("BufferFull") != std::string::npos) {
        ctx->bufferfull_count += 1;
        *buffer_full = true;
        return false;
    }

    mp_warn(ctx->log, "Starfish %s Feed returned %s; retrying\n",
            stream == STARFISH_STREAM_VIDEO ? "video" : "audio",
            result.empty() ? "(empty)" : result.c_str());
    return false;
}

enum class feed_attempt_result {
    NO_PACKET,
    SUBMITTED,
    BLOCKED,
};

static feed_attempt_result try_drain_stream(struct starfish_ctx *ctx,
                                            std::unique_lock<std::mutex> &lock,
                                            enum starfish_stream_type stream)
{
    std::deque<queued_packet> *queue = stream == STARFISH_STREAM_VIDEO
                                       ? &ctx->video_queue
                                       : &ctx->audio_queue;
    std::deque<queued_packet> *inflight = stream == STARFISH_STREAM_VIDEO
                                          ? &ctx->video_inflight
                                          : &ctx->audio_inflight;
    size_t *queue_bytes = stream == STARFISH_STREAM_VIDEO
                          ? &ctx->video_queue_bytes
                          : &ctx->audio_queue_bytes;
    size_t *inflight_bytes = stream == STARFISH_STREAM_VIDEO
                             ? &ctx->video_inflight_bytes
                             : &ctx->audio_inflight_bytes;
    const size_t inflight_limit = stream == STARFISH_STREAM_VIDEO
                                  ? VIDEO_INFLIGHT_LIMIT
                                  : AUDIO_INFLIGHT_LIMIT;
    starfish_wakeup_cb cb = stream == STARFISH_STREAM_VIDEO
                            ? ctx->video_wakeup
                            : ctx->audio_wakeup;
    void *opaque = stream == STARFISH_STREAM_VIDEO
                   ? ctx->video_wakeup_opaque
                   : ctx->audio_wakeup_opaque;

    if (queue->empty())
        return feed_attempt_result::NO_PACKET;

    queued_packet packet = queue->front();
    lock.unlock();
    bool buffer_full = false;
    bool ok = try_feed_packet(ctx, stream, packet, &buffer_full);
    lock.lock();

    if (ok) {
        *queue_bytes -= packet.data->size();
        *inflight_bytes += packet.data->size();
        inflight->push_back(packet);
        while (*inflight_bytes > inflight_limit && !inflight->empty()) {
            *inflight_bytes -= inflight->front().data->size();
            inflight->pop_front();
        }
        queue->pop_front();
        lock.unlock();
        if (cb)
            cb(opaque);
        lock.lock();
        return feed_attempt_result::SUBMITTED;
    }

    return feed_attempt_result::BLOCKED;
}

static void apply_flush(struct starfish_ctx *ctx)
{
    bool loaded = false;
    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        loaded = is_loaded_state(ctx->state);
    }

    if (loaded && !media_call_bool(ctx, "flush", [&] { return ctx->media->flush(); }))
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
        if (ctx->pending_seek_target) {
            ctx->segment_target_pts_ns = ctx->pending_seek_target_pts_ns;
            ctx->have_segment_target = true;
        } else {
            ctx->segment_target_pts_ns = 0;
            ctx->have_segment_target = false;
        }
        ctx->need_segment = true;
        ctx->flush_requested = false;
        ctx->flush_count += 1;
        mp_info(ctx->log,
                "Starfish apply_flush #%" PRIu64 ": have_target=%d target=%" PRId64 "\n",
                ctx->flush_count, ctx->have_segment_target, ctx->segment_target_pts_ns);
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
                .dolby_vision = ctx->effective_dovi,
                .dolby_vision_profile = ctx->dv_profile,
                .dolby_vision_dual_layer = dovi_track_is_dual_layer(ctx),
                .audio_channels = ctx->audio_channels,
                .audio_profile = ctx->audio_profile,
                .audio_samplerate = ctx->audio_samplerate,
                .audio_raw = ctx->audio_raw,
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

            mp_info(ctx->log,
                    "Starting Starfish load: video=%s audio=%s size=%dx%d fps=%d/%d "
                    "window=%s dovi=%d profile=%u policy=%s\n",
                    params.video_codec ? params.video_codec : "(none)",
                    params.audio_codec ? params.audio_codec : "(none)",
                    params.width, params.height, params.fps_num, params.fps_den,
                    params.window_id ? params.window_id : "(acb)",
                    params.dolby_vision, ctx->dv_profile,
                    dovi_policy_name(ctx->dovi_mode));
            mp_verbose(ctx->log, "Starfish Load payload: %s\n", payload.c_str());

            lock.unlock();
            if (!ensure_media(ctx)) {
                lock.lock();
                set_failed_locked(ctx, -1, "StarfishMediaAPIs allocation failed");
                wake_all(ctx);
                continue;
            }
            if (!media_call_bool(ctx, "notifyForeground",
                                 [&] { return ctx->media->notifyForeground(); }))
                mp_warn(ctx->log, "Starfish notifyForeground failed\n");
            bool ok = media_call_bool(ctx, "Load",
                                      [&] { return ctx->media->Load(payload.c_str(), &player_callback, ctx); });
            mp_info(ctx->log, "Starfish Load returned: %s\n", ok ? "success" : "failure");
            if (ok)
                apply_hdr_info(ctx);
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
                if (!media_call_bool(ctx, "Pause", [&] { return ctx->media->Pause(); }))
                    mp_warn(ctx->log, "Starfish Pause failed\n");
                lock.lock();
                continue;
            }

            if (ctx->play_requested && ctx->state == pipeline_state::PAUSED) {
                lock.unlock();
                if (!media_call_bool(ctx, "Play", [&] { return ctx->media->Play(); }))
                    mp_warn(ctx->log, "Starfish Play failed\n");
                lock.lock();
                continue;
            }

            if (!ctx->video_queue.empty() || !ctx->audio_queue.empty()) {
                const bool have_video = !ctx->video_queue.empty();
                const bool have_audio = !ctx->audio_queue.empty();
                const bool prefer_audio =
                    have_audio &&
                    (!have_video ||
                     ctx->audio_queue.front().pts_ns <= ctx->video_queue.front().pts_ns);
                const enum starfish_stream_type first =
                    prefer_audio ? STARFISH_STREAM_AUDIO : STARFISH_STREAM_VIDEO;
                const enum starfish_stream_type second =
                    prefer_audio ? STARFISH_STREAM_VIDEO : STARFISH_STREAM_AUDIO;

                bool blocked = false;
                feed_attempt_result result = try_drain_stream(ctx, lock, first);
                if (result == feed_attempt_result::SUBMITTED)
                    continue;
                blocked |= result == feed_attempt_result::BLOCKED;

                result = try_drain_stream(ctx, lock, second);
                if (result == feed_attempt_result::SUBMITTED)
                    continue;
                blocked |= result == feed_attempt_result::BLOCKED;

                if (blocked) {
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    lock.lock();
                    continue;
                }
            }

            if (ctx->eos_pending && !ctx->eos_sent) {
                ctx->eos_sent = true;
                lock.unlock();
                if (!media_call_bool(ctx, "pushEOS", [&] { return ctx->media->pushEOS(); }))
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
                   (ctx->play_requested && ctx->state == pipeline_state::PAUSED) ||
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
        case PF_EVENT_TYPE_STR_STATE_UPDATE__SEEKDONE:
            ctx->seekdone_count += 1;
            wake_video = true;
            wake_audio = true;
            break;
        case PF_EVENT_TYPE_INT_BUFFER_RANGE_INFO:
            mp_info(ctx->log, "Starfish buffer range info=%" PRId64 "\n", numValue);
            break;
        case PF_EVENT_TYPE_INT_BUFFERLOW:
        case PF_EVENT_TYPE_STR_BUFFERLOW:
            ctx->bufferlow_count += 1;
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
            ctx->pending_seek_target = false;
            ctx->pending_seek_target_pts_ns = 0;
            ctx->have_segment_target = false;
            ctx->segment_target_pts_ns = 0;
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

    if (call_play && !media_call_bool(ctx, "Play after load", [&] { return ctx->media->Play(); }))
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
    if (env_wants_audio_hint()) {
        ctx->audio_codec = "AAC";
        ctx->audio_channels = 2;
        ctx->audio_samplerate = 48000;
        ctx->audio_profile = AV_PROFILE_AAC_LOW;
        ctx->audio_raw = true;
        ctx->need_audio = true;
    }
    ctx->worker = std::thread(worker_loop, ctx);
    return ctx;
}

bool starfish_ctx_prime_media(void)
{
    std::lock_guard<std::mutex> media_lock(g_media_init_lock);
    if (g_primed_media)
        return true;
    g_primed_media = create_media_instance(nullptr);
    return !!g_primed_media;
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
        media_call_bool(ctx, "Unload", [&] { return ctx->media->Unload(); });
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

    const AVDOVIDecoderConfigurationRecord *dovi = find_dovi_config(codec);
    std::lock_guard<std::mutex> lock(ctx->lock);
    if (ctx->state == pipeline_state::LOADING || is_loaded_state(ctx->state))
        return false;

    ctx->dovi_mode = get_dovi_policy();
    ctx->video_codec = name;
    ctx->video_codec_tag = codec->codec_tag;
    ctx->video_color = codec->color;
    ctx->video_repr = codec->repr;
    ctx->source_dovi = codec->dovi;
    ctx->dv_profile = codec->dv_profile;
    ctx->dv_level = codec->dv_level;
    ctx->dv_rpu_present = false;
    ctx->dv_el_present = false;
    ctx->dv_bl_present = false;
    ctx->dv_bl_signal_compatibility_id = 0;
    if (dovi) {
        ctx->source_dovi = true;
        ctx->dv_profile = dovi->dv_profile;
        ctx->dv_level = dovi->dv_level;
        ctx->dv_rpu_present = dovi->rpu_present_flag;
        ctx->dv_el_present = dovi->el_present_flag;
        ctx->dv_bl_present = dovi->bl_present_flag;
        ctx->dv_bl_signal_compatibility_id = dovi->dv_bl_signal_compatibility_id;
    }
    ctx->effective_dovi = resolve_effective_dovi_locked(ctx);
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
    mp_info(ctx->log,
            "Configured Starfish video: codec=%s codec_tag=0x%x trc=%d dovi=%d/%d "
            "profile=%u level=%u compat=%u policy=%s\n",
            ctx->video_codec.c_str(), ctx->video_codec_tag, ctx->video_color.transfer,
            ctx->source_dovi, ctx->effective_dovi, ctx->dv_profile, ctx->dv_level,
            ctx->dv_bl_signal_compatibility_id, dovi_policy_name(ctx->dovi_mode));
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
    if (ctx->audio_codec == name && ctx->need_audio)
        return true;
    if (ctx->state == pipeline_state::LOADING || is_loaded_state(ctx->state))
        return false;
    ctx->audio_codec = name;
    ctx->need_audio = true;
    return true;
}

bool starfish_ctx_configure_audio_aac(struct starfish_ctx *ctx, int channels,
                                      int samplerate, int profile, bool raw)
{
    std::lock_guard<std::mutex> lock(ctx->lock);
    if (ctx->audio_codec == "AAC" &&
        ctx->audio_channels == channels &&
        ctx->audio_samplerate == samplerate &&
        ctx->audio_profile == profile &&
        ctx->audio_raw == raw &&
        ctx->need_audio)
    {
        return true;
    }
    if (ctx->state == pipeline_state::LOADING || is_loaded_state(ctx->state))
        return false;
    ctx->audio_codec = "AAC";
    ctx->audio_channels = channels;
    ctx->audio_samplerate = samplerate;
    ctx->audio_profile = profile;
    ctx->audio_raw = raw;
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

bool starfish_ctx_set_seek_target(struct starfish_ctx *ctx, double pts)
{
    if (pts == MP_NOPTS_VALUE)
        return true;

    std::lock_guard<std::mutex> lock(ctx->lock);
    ctx->pending_seek_target = true;
    ctx->pending_seek_target_pts_ns = (int64_t)(pts * 1e9);
    if (ctx->need_segment) {
        ctx->segment_target_pts_ns = ctx->pending_seek_target_pts_ns;
        ctx->have_segment_target = true;
    }
    return true;
}

bool starfish_ctx_flush(struct starfish_ctx *ctx, double pts)
{
    {
        std::lock_guard<std::mutex> lock(ctx->lock);
        ctx->flush_requested = true;
        if (pts != MP_NOPTS_VALUE) {
            ctx->flush_pts_ns = (int64_t)(pts * 1e9);
            ctx->pending_seek_target = true;
            ctx->pending_seek_target_pts_ns = ctx->flush_pts_ns;
        } else if (!ctx->pending_seek_target) {
            ctx->segment_target_pts_ns = 0;
            ctx->have_segment_target = false;
        }
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
