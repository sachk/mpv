#include "starfish_ctx.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <inttypes.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <appswitching-control-block/AcbAPI.h>
#include <glib.h>
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

/* ===================================================================
 * starfish_ctx — queued Starfish media pipeline interface
 *
 * mpv decoder/audio threads enqueue packet copies and return quickly.
 * A single worker thread owns StarfishMediaAPIs::Load(), segment restarts,
 * Feed() retry/backpressure, EOS, and Play()/Pause() transitions. This keeps
 * preroll packets alive while Starfish moves through LOADING and prevents
 * startup from dropping the first packets that carry HDR/DOVI metadata.
 *
 * Lifecycle matches Kodi's webOS integration:
 *   1. Load() called on first video feed when config is ready
 *   2. LOADCOMPLETED callback → Play() once
 *   3. flush() → clears queued preroll and marks a segment restart;
 *      pipeline stays PLAYING (no Pause before, no Play after)
 *   4. First video feed after flush → setTimeToDecode(packet_pts) +
 *      sendSegmentEvent; Starfish restarts decoding from the new position
 * =================================================================== */

namespace {

constexpr unsigned int MIN_SRC_BUFFER_LEVEL_AUDIO = 1 * 1024 * 1024;
constexpr unsigned int MIN_SRC_BUFFER_LEVEL_VIDEO = 1 * 1024 * 1024;
constexpr unsigned int MAX_SRC_BUFFER_LEVEL_AUDIO = 2 * 1024 * 1024;
constexpr unsigned int MAX_SRC_BUFFER_LEVEL_VIDEO = 8 * 1024 * 1024;
constexpr size_t VIDEO_QUEUE_LIMIT = MAX_SRC_BUFFER_LEVEL_VIDEO;
constexpr size_t AUDIO_QUEUE_LIMIT = MAX_SRC_BUFFER_LEVEL_AUDIO;
constexpr int64_t MAX_FEED_AHEAD_NS = 1600LL * 1000 * 1000;
constexpr int64_t STALE_READY_TOLERANCE_NS = 1000LL * 1000;
constexpr int64_t READY_CEILING_SLACK_NS = 5LL * 1000 * 1000 * 1000;

enum class pipeline_state {
  IDLE,
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
  bool keyframe = false;
};

struct wake_target {
  starfish_wakeup_cb cb = nullptr;
  void *opaque = nullptr;
};

struct callback_guard {
  std::atomic<starfish_ctx *> ctx{nullptr};
  std::atomic<int> active{0};
};

const char *get_app_id() {
  const char *app_id = getenv("APPID");
  return app_id && app_id[0] ? app_id : "mpv";
}

bool env_wants_audio_hint() {
  const char *hint = getenv("STARFISH_AUDIO_HINT");
  return hint && hint[0] && strcmp(hint, "0") != 0;
}

dovi_policy get_dovi_policy() {
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

const char *dovi_policy_name(dovi_policy policy) {
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

const char *video_codec_name(enum AVCodecID codec) {
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

const char *audio_codec_name_from_format(int format) {
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
                  long playState, const char *reply) {}

} // namespace

struct starfish_ctx {
  std::atomic<int> refs{1};
  callback_guard *callbacks = nullptr;
  struct mp_log *log = nullptr;
  std::mutex lock;
  std::condition_variable cv;
  std::thread worker;
  bool stop = false;
  bool resetting = false;
  int media_calls = 0;

  std::unique_ptr<StarfishMediaAPIs> media;

  /* window / ACB */
  std::string window_id;
  long acb_id = 0;
  long acb_task_id = 0;
  bool acb_initialized = false;

  /* video config */
  std::string video_codec;
  unsigned int video_codec_tag = 0;
  int width = 0;
  int height = 0;
  double fps = 0.0;
  int fps_num = 0;
  int fps_den = 1;

  /* audio config */
  std::string audio_codec;
  int audio_channels = 0;
  int audio_samplerate = 0;
  int audio_profile = 0;
  bool audio_raw = false;
  bool need_audio = false;

  /* dovi / hdr */
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

  /* pipeline state */
  pipeline_state state = pipeline_state::IDLE;
  bool play_requested = true;
  bool need_segment = false;
  bool flush_requested = false;
  int64_t flush_pts_ns = 0;
  bool pending_seek_target = false;
  int64_t pending_seek_target_ns = 0;
  bool seek_target_valid = false;
  int64_t seek_target_ns = 0;
  bool eos_pushed = false;
  bool eos_pending = false;
  bool ended = false;
  bool started = false;
  int64_t current_pts_ns = 0;
  int64_t min_ready_pts_ns = INT64_MIN;
  int64_t fed_video_pts_ns = INT64_MIN;
  int64_t fed_audio_pts_ns = INT64_MIN;
  bool pts_offset_valid = false;
  int64_t pts_offset_ns = 0;
  int video_bufferfull_logs = 0;
  int audio_bufferfull_logs = 0;
  bool audio_prime_requested = false;

  std::deque<queued_packet> video_queue;
  std::deque<queued_packet> audio_queue;
  size_t video_queue_bytes = 0;
  size_t audio_queue_bytes = 0;

  /* ready frames for vo */
  std::deque<struct starfish_video_frame> ready_frames;
  /* wakeup callbacks */
  starfish_wakeup_cb video_wakeup = nullptr;
  void *video_wakeup_opaque = nullptr;
  starfish_wakeup_cb audio_wakeup = nullptr;
  void *audio_wakeup_opaque = nullptr;
  starfish_audio_prime_cb audio_prime = nullptr;
  void *audio_prime_opaque = nullptr;
};

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static std::mutex g_current_lock;
static struct starfish_ctx *g_current_ctx;
static std::mutex g_media_init_lock;
static std::unique_ptr<StarfishMediaAPIs> g_primed_media;
static void player_callback(int32_t type, int64_t numValue,
                            const char *strValue, void *opaque);
static void worker_loop(struct starfish_ctx *ctx);

template <typename F>
static bool media_call_bool(struct starfish_ctx *ctx, const char *what,
                            F &&fn) {
  {
    std::lock_guard<std::mutex> lk(ctx->lock);
    ctx->media_calls++;
  }
  bool result = false;
  try {
    result = fn();
  } catch (const std::exception &e) {
    mp_err(ctx->log, "Starfish %s threw exception: %s\n", what, e.what());
  } catch (...) {
    mp_err(ctx->log, "Starfish %s threw unknown exception\n", what);
  }
  {
    std::lock_guard<std::mutex> lk(ctx->lock);
    ctx->media_calls--;
  }
  ctx->cv.notify_all();
  return result;
}

template <typename F>
static std::string media_call_string(struct starfish_ctx *ctx, const char *what,
                                     F &&fn) {
  {
    std::lock_guard<std::mutex> lk(ctx->lock);
    ctx->media_calls++;
  }
  std::string result;
  try {
    result = fn();
  } catch (const std::exception &e) {
    mp_err(ctx->log, "Starfish %s threw exception: %s\n", what, e.what());
  } catch (...) {
    mp_err(ctx->log, "Starfish %s threw unknown exception\n", what);
  }
  {
    std::lock_guard<std::mutex> lk(ctx->lock);
    ctx->media_calls--;
  }
  ctx->cv.notify_all();
  return result;
}

static const char *event_name(int32_t type) {
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

static bool is_loaded_state(pipeline_state state) {
  return state == pipeline_state::LOADED || state == pipeline_state::PLAYING ||
         state == pipeline_state::PAUSED;
}

static void wake_stream(struct starfish_ctx *ctx,
                        enum starfish_stream_type stream) {
  starfish_wakeup_cb cb = nullptr;
  void *opaque = nullptr;
  {
    std::lock_guard<std::mutex> lk(ctx->lock);
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

static wake_target get_wake_target_locked(struct starfish_ctx *ctx,
                                          enum starfish_stream_type stream) {
  if (stream == STARFISH_STREAM_VIDEO)
    return {ctx->video_wakeup, ctx->video_wakeup_opaque};
  return {ctx->audio_wakeup, ctx->audio_wakeup_opaque};
}

static void wake_all(struct starfish_ctx *ctx) {
  wake_stream(ctx, STARFISH_STREAM_VIDEO);
  wake_stream(ctx, STARFISH_STREAM_AUDIO);
}

static bool request_audio_prime(struct starfish_ctx *ctx, int64_t pts_ns) {
  starfish_audio_prime_cb cb = nullptr;
  void *opaque = nullptr;
  {
    std::lock_guard<std::mutex> lk(ctx->lock);
    cb = ctx->audio_prime;
    opaque = ctx->audio_prime_opaque;
  }
  if (!cb)
    return false;
  return cb(opaque, pts_ns);
}

static std::unique_ptr<StarfishMediaAPIs>
create_media_instance(struct mp_log *log) {
  try {
    return std::make_unique<StarfishMediaAPIs>();
  } catch (const std::exception &e) {
    if (log)
      mp_err(log, "StarfishMediaAPIs allocation threw exception: %s\n",
             e.what());
  } catch (...) {
    if (log)
      mp_err(log, "StarfishMediaAPIs allocation threw unknown exception\n");
  }
  return nullptr;
}

static bool ensure_media(struct starfish_ctx *ctx) {
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

/* ------------------------------------------------------------------ */
/* dovi helpers                                                        */
/* ------------------------------------------------------------------ */

static const AVDOVIDecoderConfigurationRecord *
find_dovi_config(const struct mp_codec_params *codec) {
  if (!codec || !codec->lav_codecpar)
    return nullptr;
  for (int n = 0; n < codec->lav_codecpar->nb_coded_side_data; n++) {
    const AVPacketSideData *sd = &codec->lav_codecpar->coded_side_data[n];
    if (sd->type == AV_PKT_DATA_DOVI_CONF &&
        sd->size >= (int)sizeof(AVDOVIDecoderConfigurationRecord))
      return reinterpret_cast<const AVDOVIDecoderConfigurationRecord *>(
          sd->data);
  }
  return nullptr;
}

static bool resolve_effective_dovi(struct starfish_ctx *ctx) {
  if (!ctx->source_dovi)
    return false;
  switch (ctx->dovi_mode) {
  case dovi_policy::HDR10:
    return false;
  case dovi_policy::PASSTHROUGH:
    return true;
  case dovi_policy::AUTO:
  case dovi_policy::P7_FALLBACK:
    if (ctx->dv_profile == 7 && ctx->dv_el_present)
      mp_info(ctx->log,
              "Starfish: Profile 7 detected, allowing conversion to 8.1\n");
    return true;
  }
  return false;
}

/* ------------------------------------------------------------------ */
/* hdr helpers                                                         */
/* ------------------------------------------------------------------ */

static const char *hdr_type_name(const struct starfish_ctx *ctx) {
  switch (ctx->video_color.transfer) {
  case PL_COLOR_TRC_PQ:
    return "hdr10";
  case PL_COLOR_TRC_HLG:
    return "hlg";
  default:
    return "none";
  }
}

static bool hdr_sei_available(const struct starfish_ctx *ctx) {
  const struct pl_hdr_metadata *hdr = &ctx->video_color.hdr;
  const struct pl_raw_primaries *prim = &hdr->prim;
  return hdr->min_luma > 0.0f || hdr->max_luma > 0.0f || hdr->max_cll > 0.0f ||
         hdr->max_fall > 0.0f || prim->red.x > 0.0f || prim->green.x > 0.0f ||
         prim->blue.x > 0.0f || prim->white.x > 0.0f;
}

static int scale_chromaticity(float v) {
  return v > 0.0f ? (int)llrintf(v * 50000.0f) : 0;
}
static int scale_luminance(float v) {
  return v > 0.0f ? (int)llrintf(v * 10000.0f) : 0;
}
static int scale_content_light(float v) {
  return v > 0.0f ? (int)llrintf(v) : 0;
}

static enum AVColorSpace color_system_to_av(enum pl_color_system sys) {
  switch (sys) {
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
  case PL_COLOR_SYSTEM_YCGCO:
    return AVCOL_SPC_YCGCO;
  case PL_COLOR_SYSTEM_RGB:
    return AVCOL_SPC_RGB;
  default:
    return AVCOL_SPC_UNSPECIFIED;
  }
}

static enum AVColorRange color_levels_to_av(enum pl_color_levels levels) {
  switch (levels) {
  case PL_COLOR_LEVELS_LIMITED:
    return AVCOL_RANGE_MPEG;
  case PL_COLOR_LEVELS_FULL:
    return AVCOL_RANGE_JPEG;
  default:
    return AVCOL_RANGE_UNSPECIFIED;
  }
}

static enum AVColorPrimaries
color_primaries_to_av(enum pl_color_primaries prim) {
  switch (prim) {
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
  default:
    return AVCOL_PRI_UNSPECIFIED;
  }
}

static enum AVColorTransferCharacteristic
color_transfer_to_av(enum pl_color_transfer trc) {
  switch (trc) {
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
  default:
    return AVCOL_TRC_UNSPECIFIED;
  }
}

static void apply_hdr_info(struct starfish_ctx *ctx) {
  const char *hdr_type = hdr_type_name(ctx);
  if (strcmp(hdr_type, "none") == 0 || !hdr_sei_available(ctx))
    return;

  const struct pl_hdr_metadata *hdr = &ctx->video_color.hdr;
  struct starfish_json_hdr_info_params params = {
      .hdr_type = hdr_type,
      .has_sei = true,
      .display_primaries_x0 = scale_chromaticity(hdr->prim.green.x),
      .display_primaries_y0 = scale_chromaticity(hdr->prim.green.y),
      .display_primaries_x1 = scale_chromaticity(hdr->prim.blue.x),
      .display_primaries_y1 = scale_chromaticity(hdr->prim.blue.y),
      .display_primaries_x2 = scale_chromaticity(hdr->prim.red.x),
      .display_primaries_y2 = scale_chromaticity(hdr->prim.red.y),
      .white_point_x = scale_chromaticity(hdr->prim.white.x),
      .white_point_y = scale_chromaticity(hdr->prim.white.y),
      .min_display_mastering_luminance = scale_luminance(hdr->min_luma),
      .max_display_mastering_luminance = scale_luminance(hdr->max_luma),
      .max_content_light_level = scale_content_light(hdr->max_cll),
      .max_pic_average_light_level = scale_content_light(hdr->max_fall),
      .transfer_characteristics =
          color_transfer_to_av(ctx->video_color.transfer),
      .color_primaries = color_primaries_to_av(ctx->video_color.primaries),
      .matrix_coeffs = color_system_to_av(ctx->video_repr.sys),
      .video_full_range_flag =
          color_levels_to_av(ctx->video_repr.levels) == AVCOL_RANGE_JPEG,
  };

  std::string payload = starfish_json_build_hdr_info(&params);
  mp_info(ctx->log, "Starfish setHdrInfo payload: %s\n", payload.c_str());
  if (!media_call_bool(ctx, "setHdrInfo",
                       [&] { return ctx->media->setHdrInfo(payload.c_str()); }))
    mp_warn(ctx->log, "Starfish setHdrInfo failed\n");
}

/* ------------------------------------------------------------------ */
/* ACB                                                                 */
/* ------------------------------------------------------------------ */

static bool ensure_acb(struct starfish_ctx *ctx) {
  if (!ctx->window_id.empty() || ctx->acb_id)
    return true;
  ctx->acb_id = AcbAPI_create();
  if (!ctx->acb_id) {
    mp_err(ctx->log, "AcbAPI_create failed\n");
    return false;
  }
  if (!AcbAPI_initialize(ctx->acb_id, PLAYER_TYPE_MSE, get_app_id(),
                         &acb_callback)) {
    mp_err(ctx->log, "AcbAPI_initialize failed\n");
    AcbAPI_destroy(ctx->acb_id);
    ctx->acb_id = 0;
    return false;
  }
  ctx->acb_initialized = true;
  return true;
}

/* ------------------------------------------------------------------ */
/* pipeline helpers (setTimeToDecode, sendSegmentEvent)                 */
/* ------------------------------------------------------------------ */

static bool set_time_to_decode(struct starfish_ctx *ctx, int64_t pts_ns) {
  std::string payload = starfish_json_build_seek(pts_ns);
  if (media_call_bool(ctx, "setTimeToDecode", [&] {
        return ctx->media->setTimeToDecode(payload.c_str());
      }))
    return true;

  /* fallback: set ptsToDecode via pipeline contentInfo */
  auto *player =
      static_cast<mediapipeline::CustomPlayer *>(ctx->media->player.get());
  auto *pipeline = player ? static_cast<mediapipeline::CustomPipeline *>(
                                player->getPipeline().get())
                          : nullptr;
  if (!pipeline)
    return false;

  MEDIA_CUSTOM_CONTENT_INFO_T content_info;
  try {
    pipeline->loadSpi_getInfo(&content_info);
  } catch (...) {
    return false;
  }
  content_info.ptsToDecode = pts_ns;
  try {
    pipeline->setContentInfo(MEDIA_CUSTOM_SRC_TYPE_ES, &content_info);
  } catch (...) {
    return false;
  }
  return true;
}

static bool send_segment_event(struct starfish_ctx *ctx) {
  auto *player =
      static_cast<mediapipeline::CustomPlayer *>(ctx->media->player.get());
  auto *pipeline = player ? static_cast<mediapipeline::CustomPipeline *>(
                                player->getPipeline().get())
                          : nullptr;
  if (!pipeline)
    return true;
  try {
    pipeline->sendSegmentEvent();
    return true;
  } catch (...) {
    mp_err(ctx->log, "Starfish sendSegmentEvent threw exception\n");
    return false;
  }
}

static void prime_custom_pipeline(struct starfish_ctx *ctx) {
  auto *player =
      static_cast<mediapipeline::CustomPlayer *>(ctx->media->player.get());
  auto *pipeline = player ? static_cast<mediapipeline::CustomPipeline *>(
                                player->getPipeline().get())
                          : nullptr;
  if (!pipeline) {
    mp_warn(ctx->log, "Starfish CustomPipeline unavailable after load\n");
    return;
  }

  MEDIA_CUSTOM_CONTENT_INFO_T content_info;
  try {
    pipeline->loadSpi_getInfo(&content_info);
    mp_info(ctx->log, "Starfish CustomPipeline primed\n");
  } catch (...) {
    mp_warn(ctx->log, "Starfish CustomPipeline prime failed\n");
  }
}

/* ------------------------------------------------------------------ */
/* load config check & trigger                                         */
/* ------------------------------------------------------------------ */

static bool have_load_config(struct starfish_ctx *ctx) {
  return !ctx->video_codec.empty() && ctx->width > 0 && ctx->height > 0 &&
         ctx->fps_num > 0 && ctx->fps_den > 0;
}

static bool can_load(struct starfish_ctx *ctx) {
  if (ctx->resetting)
    return false;
  if (ctx->state != pipeline_state::IDLE)
    return false;
  if (!have_load_config(ctx))
    return false;
  if (ctx->video_queue.empty())
    return false;
  if (ctx->need_audio && ctx->audio_queue.empty())
    return false;
  return !ctx->window_id.empty() || ensure_acb(ctx);
}

static void prepare_segment_timeline_locked(struct starfish_ctx *ctx,
                                            int64_t start_pts_ns,
                                            const char *reason);

/* Try to start Load(). Called with lock held. Releases lock during Load().
 * Returns true if Load() was started (state is now LOADING or LOADED/PLAYING).
 */
static bool try_start_load(struct starfish_ctx *ctx,
                           std::unique_lock<std::mutex> &lk) {
  if (!can_load(ctx))
    return false;

  struct starfish_json_load_params params = {
      .app_id = get_app_id(),
      .window_id = ctx->window_id.empty() ? nullptr : ctx->window_id.c_str(),
      .video_codec = ctx->video_codec.c_str(),
      .audio_codec = ctx->need_audio ? ctx->audio_codec.c_str() : nullptr,
      .dolby_vision = ctx->effective_dovi,
      .dolby_vision_profile = ctx->dv_profile,
      .dolby_vision_dual_layer = ctx->dv_el_present,
      .audio_channels = ctx->audio_channels,
      .audio_profile = ctx->audio_profile,
      .audio_samplerate = ctx->audio_samplerate,
      .audio_raw = ctx->audio_raw,
      .width = ctx->width,
      .height = ctx->height,
      .fps_num = ctx->fps_num,
      .fps_den = ctx->fps_den,
      .max_width = 0,
      .max_height = 0,
      .max_framerate = 0,
      .adaptive_resolution = false,
      .pts_to_decode_ns =
          ctx->pending_seek_target ? ctx->pending_seek_target_ns : 0,
      .need_audio = ctx->need_audio,
  };

  std::string payload = starfish_json_build_load(&params);
  ctx->state = pipeline_state::LOADING;
  ctx->ended = false;
  ctx->eos_pushed = false;
  ctx->eos_pending = false;
  prepare_segment_timeline_locked(ctx, params.pts_to_decode_ns,
                                  "initial-load");
  mp_info(ctx->log,
          "Starfish Load: video=%s audio=%s size=%dx%d fps=%d/%d window=%s "
          "dovi=%d pts_to_decode=%" PRId64 "\n",
          params.video_codec,
          params.audio_codec ? params.audio_codec : "(none)", params.width,
          params.height, params.fps_num, params.fps_den,
          params.window_id ? params.window_id : "(acb)", params.dolby_vision,
          params.pts_to_decode_ns);

  lk.unlock();

  if (!ensure_media(ctx)) {
    lk.lock();
    ctx->state = pipeline_state::FAILED;
    return false;
  }
  media_call_bool(ctx, "notifyForeground",
                  [&] { return ctx->media->notifyForeground(); });
  bool ok = media_call_bool(ctx, "Load", [&] {
    return ctx->media->Load(payload.c_str(), &player_callback, ctx->callbacks);
  });
  mp_info(ctx->log, "Starfish Load returned: %s\n", ok ? "success" : "failure");
  if (ok)
    apply_hdr_info(ctx);

  lk.lock();
  if (!ok)
    ctx->state = pipeline_state::FAILED;
  return ok;
}

static void set_failed_locked(struct starfish_ctx *ctx, int code,
                              const char *reason) {
  ctx->state = pipeline_state::FAILED;
  mp_err(ctx->log, "Starfish failed code=%d reason=%s\n", code,
         reason ? reason : "");
}

static bool try_feed_packet(struct starfish_ctx *ctx,
                            enum starfish_stream_type stream,
                            const queued_packet &packet, bool do_segment,
                            int64_t seek_target_ns, bool *buffer_full) {
  if (do_segment) {
    int64_t target_ns = packet.pts_ns;
    mp_info(ctx->log,
            "Starfish segment restart stream=%s packet_pts=%" PRId64
            " seek_target=%" PRId64 " decode_target=%" PRId64
            " keyframe=%d\n",
            stream == STARFISH_STREAM_VIDEO ? "video" : "audio", packet.pts_ns,
            seek_target_ns, target_ns, packet.keyframe);
    if (!set_time_to_decode(ctx, target_ns))
      mp_warn(ctx->log,
              "Starfish setTimeToDecode failed for target %" PRId64 "\n",
              target_ns);
    if (!send_segment_event(ctx))
      mp_warn(ctx->log, "Starfish sendSegmentEvent failed\n");

    mp_info(ctx->log, "Starfish segment restart sent\n");
  }

  std::string payload = starfish_json_build_feed(
      stream, packet.data->data(), packet.data->size(), packet.pts_ns);
  std::string result = media_call_string(
      ctx, "Feed", [&] { return ctx->media->Feed(payload.c_str()); });

  if (result.find("Ok") != std::string::npos) {
    mp_trace(ctx->log, "Starfish %s feed: size=%zu pts=%" PRId64 " result=Ok\n",
             stream == STARFISH_STREAM_VIDEO ? "video" : "audio",
             packet.data->size(), packet.pts_ns);
    return true;
  }

  if (result.find("BufferFull") != std::string::npos) {
    *buffer_full = true;
    return false;
  }

  mp_info(ctx->log, "Starfish %s feed: size=%zu pts=%" PRId64 " result=%s\n",
          stream == STARFISH_STREAM_VIDEO ? "video" : "audio",
          packet.data->size(), packet.pts_ns, result.c_str());

  if (result.find("Error") != std::string::npos) {
    mp_warn(ctx->log,
            "Starfish %s Feed failed with Error (dropping packet) pts=%" PRId64
            " size=%zu result=%s payload=%s\n",
            stream == STARFISH_STREAM_VIDEO ? "video" : "audio", packet.pts_ns,
            packet.data->size(), result.c_str(), payload.c_str());
    return true;
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
                                            std::unique_lock<std::mutex> &lk,
                                            enum starfish_stream_type stream) {
  if (ctx->need_segment && stream == STARFISH_STREAM_AUDIO &&
      !ctx->video_codec.empty())
    return feed_attempt_result::NO_PACKET;
  std::deque<queued_packet> *queue =
      stream == STARFISH_STREAM_VIDEO ? &ctx->video_queue : &ctx->audio_queue;
  size_t *queue_bytes = stream == STARFISH_STREAM_VIDEO
                            ? &ctx->video_queue_bytes
                            : &ctx->audio_queue_bytes;

  if (queue->empty())
    return feed_attempt_result::NO_PACKET;

  if (ctx->started) {
    const int64_t fed_pts = stream == STARFISH_STREAM_VIDEO
                                ? ctx->fed_video_pts_ns
                                : ctx->fed_audio_pts_ns;
    if (fed_pts != INT64_MIN &&
        fed_pts - ctx->current_pts_ns > MAX_FEED_AHEAD_NS) {
      mp_trace(ctx->log,
               "Starfish %s BLOCKED (Ahead). Queue: %zu (%.2fMB) "
               "(current=%.3f fed=%.3f)\n",
               stream == STARFISH_STREAM_VIDEO ? "video" : "audio",
               *queue_bytes, *queue_bytes / 1024.0 / 1024.0,
               (double)ctx->current_pts_ns / 1e9, (double)fed_pts / 1e9);
      return feed_attempt_result::BLOCKED;
    }
  }

  queued_packet packet = queue->front();
  const bool is_audio_only = ctx->video_codec.empty();
  const bool do_segment =
      ctx->need_segment && (stream == STARFISH_STREAM_VIDEO || is_audio_only);
  const int64_t seek_target_ns =
      ctx->pending_seek_target ? ctx->pending_seek_target_ns : -1;
  const wake_target wake = get_wake_target_locked(ctx, stream);
  bool prime_audio_for_segment = false;
  int64_t audio_prime_pts_ns = 0;

  if (do_segment) {
    ctx->need_segment = false;
    ctx->pending_seek_target = false;
    prepare_segment_timeline_locked(ctx, packet.pts_ns, "segment-packet");
    if (ctx->need_audio) {
      ctx->audio_prime_requested = true;
      prime_audio_for_segment = true;
      audio_prime_pts_ns = packet.pts_ns;
    }
  }

  lk.unlock();
  if (prime_audio_for_segment) {
    bool primed = request_audio_prime(ctx, audio_prime_pts_ns);
    lk.lock();
    mp_info(ctx->log, "Starfish requested audio segment prime pts=%.3f result=%d\n",
            (double)audio_prime_pts_ns / 1e9, primed);
    if (ctx->stop || ctx->flush_requested)
      return feed_attempt_result::BLOCKED;
    lk.unlock();
  }
  bool buffer_full = false;
  bool ok = try_feed_packet(ctx, stream, packet, do_segment,
                            seek_target_ns, &buffer_full);
  lk.lock();

  if (ctx->stop || ctx->flush_requested)
    return feed_attempt_result::BLOCKED;

  if (!ok) {
    if (buffer_full) {
      int *logs = stream == STARFISH_STREAM_VIDEO ? &ctx->video_bufferfull_logs
                                                  : &ctx->audio_bufferfull_logs;
      if (*logs < 8) {
        mp_info(ctx->log,
                "Starfish %s BufferFull packet_pts=%.3f queue=%.2fMB "
                "started=%d current=%.3f fed_v=%.3f fed_a=%.3f\n",
                stream == STARFISH_STREAM_VIDEO ? "video" : "audio",
                (double)packet.pts_ns / 1e9, *queue_bytes / 1024.0 / 1024.0,
                ctx->started, (double)ctx->current_pts_ns / 1e9,
                ctx->fed_video_pts_ns == INT64_MIN
                    ? -1.0
                    : (double)ctx->fed_video_pts_ns / 1e9,
                ctx->fed_audio_pts_ns == INT64_MIN
                    ? -1.0
                    : (double)ctx->fed_audio_pts_ns / 1e9);
        (*logs)++;
      }
      mp_trace(ctx->log,
               "Starfish %s BLOCKED (BufferFull). Queue: %zu (%.2fMB) "
               "(packet_pts=%.3f)\n",
               stream == STARFISH_STREAM_VIDEO ? "video" : "audio",
               *queue_bytes, *queue_bytes / 1024.0 / 1024.0,
               (double)packet.pts_ns / 1e9);
    }
    return feed_attempt_result::BLOCKED;
  }

  *queue_bytes -= packet.data->size();
  queue->pop_front();
  if (do_segment && stream == STARFISH_STREAM_VIDEO) {
    ctx->current_pts_ns = packet.pts_ns;
    ctx->min_ready_pts_ns = packet.pts_ns;
    mp_info(ctx->log,
            "Starfish segment decode started at %.3f present_floor=%.3f seek_target=%.3f\n",
            (double)packet.pts_ns / 1e9, (double)ctx->min_ready_pts_ns / 1e9,
            seek_target_ns >= 0 ? (double)seek_target_ns / 1e9 : -1.0);
  }
  if (stream == STARFISH_STREAM_VIDEO)
    ctx->fed_video_pts_ns = packet.pts_ns;
  else
    ctx->fed_audio_pts_ns = packet.pts_ns;

  lk.unlock();
  if (wake.cb)
    wake.cb(wake.opaque);
  lk.lock();
  return feed_attempt_result::SUBMITTED;
}

static void prepare_segment_timeline_locked(struct starfish_ctx *ctx,
                                            int64_t start_pts_ns,
                                            const char *reason) {
  ctx->started = false;
  ctx->audio_prime_requested = false;
  ctx->pts_offset_valid = false;
  ctx->pts_offset_ns = 0;
  ctx->current_pts_ns = start_pts_ns == INT64_MIN ? 0 : start_pts_ns;
  ctx->min_ready_pts_ns = start_pts_ns;
  ctx->fed_video_pts_ns = INT64_MIN;
  ctx->fed_audio_pts_ns = INT64_MIN;
  ctx->ready_frames.clear();
  mp_info(ctx->log, "Starfish segment timeline reset reason=%s target=%.3f\n",
          reason ? reason : "unknown",
          start_pts_ns == INT64_MIN ? -1.0 : (double)start_pts_ns / 1e9);
}

static void clear_queued_packets_locked(struct starfish_ctx *ctx) {
  ctx->video_queue.clear();
  ctx->audio_queue.clear();
  ctx->video_queue_bytes = 0;
  ctx->audio_queue_bytes = 0;
  prepare_segment_timeline_locked(ctx, INT64_MIN, "clear-queues");
  ctx->video_bufferfull_logs = 0;
  ctx->audio_bufferfull_logs = 0;
}

static void apply_flush(struct starfish_ctx *ctx) {
  bool loaded = false;
  {
    std::unique_lock<std::mutex> lk(ctx->lock);
    loaded = is_loaded_state(ctx->state);
    ctx->ended = false;
    ctx->eos_pushed = false;
    ctx->eos_pending = false;
    ctx->need_segment = true;
    ctx->flush_requested = false;
    clear_queued_packets_locked(ctx);
    if (ctx->seek_target_valid)
      prepare_segment_timeline_locked(ctx, ctx->seek_target_ns, "seek-flush");
  }

  if (loaded) {
    mp_info(ctx->log, "Starfish flush applied\n");
    media_call_bool(ctx, "flush", [&] { return ctx->media->flush(); });
  }

  ctx->cv.notify_all();
  wake_all(ctx);
}

static void worker_loop(struct starfish_ctx *ctx) {
  std::unique_lock<std::mutex> lk(ctx->lock);

  while (!ctx->stop) {
    if (ctx->resetting) {
      ctx->cv.wait(lk, [&] { return ctx->stop || !ctx->resetting; });
      continue;
    }

    if (ctx->flush_requested) {
      lk.unlock();
      apply_flush(ctx);
      lk.lock();
      continue;
    }

    if (can_load(ctx)) {
      if (!try_start_load(ctx, lk)) {
        if (ctx->state != pipeline_state::FAILED)
          set_failed_locked(ctx, -1, "Starfish Load did not start");
        lk.unlock();
        wake_all(ctx);
        return;
      }
      continue;
    }

    if (ctx->state == pipeline_state::FAILED) {
      ctx->cv.wait(lk, [&] {
        return ctx->stop || ctx->flush_requested ||
               ctx->state != pipeline_state::FAILED;
      });
      continue;
    }

    if (is_loaded_state(ctx->state)) {
      /* User pause/play transitions */
      if (!ctx->play_requested && ctx->state == pipeline_state::PLAYING &&
          !ctx->pending_seek_target) {
        mp_info(ctx->log, "Starfish Pause (user)\n");
        ctx->state = pipeline_state::PAUSED;
        lk.unlock();
        if (!media_call_bool(ctx, "Pause",
                             [&] { return ctx->media->Pause(); })) {
          mp_warn(ctx->log, "Starfish Pause failed\n");
          lk.lock();
          ctx->state = pipeline_state::PLAYING;
          continue;
        }
        lk.lock();
        continue;
      }

      if (ctx->play_requested && (ctx->state == pipeline_state::PAUSED ||
                                  ctx->state == pipeline_state::LOADED)) {
        mp_info(ctx->log, "Starfish Play (user)\n");
        pipeline_state old_state = ctx->state;
        ctx->state = pipeline_state::PLAYING;
        lk.unlock();
        bool play_ok =
            media_call_bool(ctx, "Play", [&] { return ctx->media->Play(); });
        std::string rate_payload = starfish_json_build_play_rate(1000, true);
        (void)media_call_bool(ctx, "SetPlayRate", [&] {
          return ctx->media->SetPlayRate(rate_payload.c_str());
        });
        lk.lock();
        if (!play_ok)
          ctx->state = old_state;
        continue;
      }

      if (!ctx->video_queue.empty() || !ctx->audio_queue.empty()) {
        const bool have_video = !ctx->video_queue.empty();
        const bool have_audio = !ctx->audio_queue.empty();
        const bool prefer_audio =
            !ctx->need_segment && have_audio &&
            (!have_video || ctx->audio_queue.front().pts_ns <=
                               ctx->video_queue.front().pts_ns);
        const enum starfish_stream_type first =
            prefer_audio ? STARFISH_STREAM_AUDIO : STARFISH_STREAM_VIDEO;
        const enum starfish_stream_type second =
            prefer_audio ? STARFISH_STREAM_VIDEO : STARFISH_STREAM_AUDIO;

        bool blocked = false;
        feed_attempt_result result = try_drain_stream(ctx, lk, first);
        if (result == feed_attempt_result::SUBMITTED)
          continue;
        blocked |= result == feed_attempt_result::BLOCKED;

        result = try_drain_stream(ctx, lk, second);
        if (result == feed_attempt_result::SUBMITTED)
          continue;
        blocked |= result == feed_attempt_result::BLOCKED;

        if (blocked) {
          lk.unlock();
          std::this_thread::sleep_for(std::chrono::milliseconds(25));
          lk.lock();
          continue;
        }
      }

      if (ctx->eos_pending && !ctx->eos_pushed) {
        ctx->eos_pushed = true;
        lk.unlock();
        if (!media_call_bool(ctx, "pushEOS",
                             [&] { return ctx->media->pushEOS(); }))
          mp_warn(ctx->log, "Starfish pushEOS failed\n");
        lk.lock();
        ctx->eos_pending = false;
        continue;
      }
    }

    ctx->cv.wait_for(lk, std::chrono::seconds(1), [&] {
      return ctx->stop || ctx->flush_requested || can_load(ctx) ||
             (is_loaded_state(ctx->state) &&
              (!ctx->video_queue.empty() || !ctx->audio_queue.empty() ||
               (ctx->eos_pending && !ctx->eos_pushed) ||
               (ctx->play_requested &&
                (ctx->state == pipeline_state::PAUSED ||
                 ctx->state == pipeline_state::LOADED)) ||
               (!ctx->play_requested &&
                ctx->state == pipeline_state::PLAYING)));
    });
  }
}

/* ------------------------------------------------------------------ */
/* player callback (runs on Starfish's thread)                         */
static void player_callback(int32_t type, int64_t numValue,
                            const char *strValue, void *opaque) {
  auto *guard = static_cast<callback_guard *>(opaque);
  if (!guard)
    return;

  guard->active.fetch_add(1, std::memory_order_acq_rel);
  struct starfish_ctx *ctx = guard->ctx.load(std::memory_order_acquire);
  if (!ctx) {
    guard->active.fetch_sub(1, std::memory_order_acq_rel);
    return;
  }

  std::unique_lock<std::mutex> lk(ctx->lock);
  if (ctx->stop) {
    lk.unlock();
    guard->active.fetch_sub(1, std::memory_order_acq_rel);
    return;
  }

  int mapped_type = type;

  if (mapped_type == PF_EVENT_TYPE_FRAMEREADY ||
      mapped_type == PF_EVENT_TYPE_STR_BUFFERFULL) {
    mp_trace(ctx->log,
             "Starfish callback type=%d(%s) num=%" PRId64 " str=%s\n",
             type, event_name(type), numValue, strValue ? strValue : "");
  } else if (type == 49) {
    mp_trace(ctx->log, "Starfish callback type=49 num=%" PRId64 "\n",
             numValue);
  } else {
    mp_dbg(ctx->log, "Starfish callback type=%d(%s) num=%" PRId64 " str=%s\n",
           type, event_name(type), numValue, strValue ? strValue : "");
  }

  bool wake_video = false;
  bool wake_audio = false;
  bool call_play = false;

  switch (mapped_type) {
  case PF_EVENT_TYPE_FRAMEREADY: {
    int64_t mapped_pts = numValue;
    if (!ctx->pts_offset_valid) {
      int64_t anchor_pts = numValue;
      if (ctx->min_ready_pts_ns != INT64_MIN)
        anchor_pts = ctx->min_ready_pts_ns;
      else if (ctx->seek_target_valid)
        anchor_pts = ctx->seek_target_ns;
      else if (ctx->fed_video_pts_ns != INT64_MIN)
        anchor_pts = ctx->fed_video_pts_ns;

      ctx->pts_offset_ns = anchor_pts - numValue;
      ctx->pts_offset_valid = true;
      mapped_pts = numValue + ctx->pts_offset_ns;
      mp_info(ctx->log,
              "Starfish frame timeline anchored raw=%" PRId64
              " anchor=%" PRId64 " offset=%" PRId64 " mapped=%" PRId64
              "\n",
              numValue, anchor_pts, ctx->pts_offset_ns, mapped_pts);
    } else {
      mapped_pts = numValue + ctx->pts_offset_ns;
    }

    if (ctx->flush_requested || ctx->need_segment ||
        ctx->state == pipeline_state::IDLE ||
        ctx->state == pipeline_state::FAILED)
      break;
    if (ctx->min_ready_pts_ns != INT64_MIN &&
        mapped_pts + STALE_READY_TOLERANCE_NS < ctx->min_ready_pts_ns) {
      ctx->current_pts_ns = mapped_pts;
      ctx->started = false;
      mp_trace(ctx->log,
               "Starfish preroll frame raw=%.3f mapped=%.3f below present "
               "floor %.3f\n",
               (double)numValue / 1e9, (double)mapped_pts / 1e9,
               (double)ctx->min_ready_pts_ns / 1e9);
      break;
    }
    if (ctx->fed_video_pts_ns != INT64_MIN &&
        mapped_pts > ctx->fed_video_pts_ns + READY_CEILING_SLACK_NS) {
      mp_info(ctx->log,
              "Starfish dropping stale frame raw=%.3f mapped=%.3f above fed "
              "ceiling %.3f\n",
              (double)numValue / 1e9, (double)mapped_pts / 1e9,
              (double)(ctx->fed_video_pts_ns + READY_CEILING_SLACK_NS) / 1e9);
      break;
    }
    struct starfish_video_frame frame = {
        .pts = mapped_pts / 1000000000.0,
        .dts = mapped_pts / 1000000000.0,
        .duration = ctx->fps > 0 ? 1.0 / ctx->fps : 0.0,
    };
    ctx->ready_frames.push_back(frame);
    ctx->current_pts_ns = mapped_pts;
    ctx->min_ready_pts_ns = INT64_MIN;
    ctx->started = true;
    wake_video = true;
    break;
  }
  case PF_EVENT_TYPE_STR_STATE_UPDATE__PRELOADCOMPLETED:
    mp_info(ctx->log, "Starfish StateUpdate: PRELOADCOMPLETED\n");
    break;
  case PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED: {
    mp_info(ctx->log, "Starfish StateUpdate: LOADCOMPLETED\n");
    ctx->state = pipeline_state::LOADED;
    ctx->need_segment = true;
    ctx->ended = false;

    long acb_id = ctx->acb_id;
    long acb_task_id = ctx->acb_task_id;
    std::string media_id = ctx->media ? ctx->media->getMediaID() : "";

    lk.unlock();
    if (ctx->media)
      prime_custom_pipeline(ctx);
    if (acb_id) {
      AcbAPI_setSinkType(acb_id, SINK_TYPE_MAIN);
      if (!media_id.empty())
        AcbAPI_setMediaId(acb_id, media_id.c_str());
      AcbAPI_setState(acb_id, APPSTATE_FOREGROUND, PLAYSTATE_LOADED,
                      &acb_task_id);
    }
    lk.lock();
    wake_video = true;
    wake_audio = true;
    break;
  }
  case PF_EVENT_TYPE_STR_STATE_UPDATE__PLAYING:
    mp_info(ctx->log, "Starfish StateUpdate: PLAYING\n");
    ctx->state = pipeline_state::PLAYING;
    if (ctx->acb_id) {
      long acb_id = ctx->acb_id;
      long acb_task_id = ctx->acb_task_id;
      lk.unlock();
      AcbAPI_setState(acb_id, APPSTATE_FOREGROUND, PLAYSTATE_PLAYING,
                      &acb_task_id);
      lk.lock();
    }
    wake_video = true;
    wake_audio = true;
    break;
  case PF_EVENT_TYPE_STR_STATE_UPDATE__PAUSED:
    mp_info(ctx->log, "Starfish StateUpdate: PAUSED\n");
    ctx->state = pipeline_state::PAUSED;
    if (ctx->acb_id) {
      long acb_id = ctx->acb_id;
      long acb_task_id = ctx->acb_task_id;
      lk.unlock();
      AcbAPI_setState(acb_id, APPSTATE_FOREGROUND, PLAYSTATE_PAUSED,
                      &acb_task_id);
      lk.lock();
    }
    break;
  case PF_EVENT_TYPE_STR_STATE_UPDATE__ENDOFSTREAM:
    mp_info(ctx->log, "Starfish StateUpdate: ENDOFSTREAM\n");
    ctx->ended = true;
    wake_video = true;
    wake_audio = true;
    break;
  case PF_EVENT_TYPE_STR_STATE_UPDATE__SEEKDONE:
    mp_info(ctx->log, "Starfish StateUpdate: SEEKDONE\n");
    wake_video = true;
    wake_audio = true;
    break;
  case PF_EVENT_TYPE_STR_STATE_UPDATE__UNLOADCOMPLETED:
    mp_info(ctx->log, "Starfish StateUpdate: UNLOADCOMPLETED\n");
    ctx->state = pipeline_state::IDLE;
    ctx->ended = true;
    ctx->eos_pushed = false;
    ctx->eos_pending = false;
    ctx->need_segment = false;
    ctx->pending_seek_target = false;
    ctx->seek_target_valid = false;
    ctx->started = false;
    ctx->min_ready_pts_ns = INT64_MIN;
    ctx->fed_video_pts_ns = INT64_MIN;
    ctx->fed_audio_pts_ns = INT64_MIN;
    ctx->pts_offset_valid = false;
    ctx->pts_offset_ns = 0;
    ctx->ready_frames.clear();
    clear_queued_packets_locked(ctx);
    if (ctx->acb_id) {
      long acb_id = ctx->acb_id;
      long acb_task_id = ctx->acb_task_id;
      lk.unlock();
      AcbAPI_setState(acb_id, APPSTATE_FOREGROUND, PLAYSTATE_UNLOADED,
                      &acb_task_id);
      lk.lock();
    }
    wake_video = true;
    wake_audio = true;
    break;
  case PF_EVENT_TYPE_INT_BUFFERLOW:
  case PF_EVENT_TYPE_STR_BUFFERLOW:
    mp_info(ctx->log, "Starfish Event: BUFFERLOW\n");
    wake_video = true;
    wake_audio = true;
    break;
  case PF_EVENT_TYPE_STR_BUFFERFULL:
    mp_dbg(ctx->log, "Starfish Event: BUFFERFULL\n");
    wake_video = true;
    wake_audio = true;
    break;
  case PF_EVENT_TYPE_STR_VIDEO_INFO:
    mp_info(ctx->log, "Starfish Event: VIDEO_INFO %s\n",
            strValue ? strValue : "");
    if (ctx->acb_id && strValue) {
      long acb_id = ctx->acb_id;
      std::string val = strValue;
      lk.unlock();
      AcbAPI_setMediaVideoData(acb_id, val.c_str());
      lk.lock();
    }
    break;
  case PF_EVENT_TYPE_STR_AUDIO_INFO:
    mp_info(ctx->log, "Starfish Event: AUDIO_INFO %s\n",
            strValue ? strValue : "");
    /* Audio metadata reporting not supported by this ACB version or function
     * named differently */
    break;
  case PF_EVENT_TYPE_INT_ERROR:
  case PF_EVENT_TYPE_STR_ERROR:
    mp_err(ctx->log, "Starfish error type=%d num=%" PRId64 " str=%s\n", type,
           numValue, strValue ? strValue : "");
    if (ctx->state == pipeline_state::LOADING)
      ctx->state = pipeline_state::FAILED;
    wake_video = true;
    wake_audio = true;
    break;
  default:
    if (type != 49)
      mp_dbg(ctx->log, "Starfish event type=%d (%s) num=%" PRId64 " str=%s\n",
             type, event_name(type), numValue, strValue ? strValue : "");
    break;
  }

  ctx->cv.notify_all();
  wake_target video_wake = get_wake_target_locked(ctx, STARFISH_STREAM_VIDEO);
  wake_target audio_wake = get_wake_target_locked(ctx, STARFISH_STREAM_AUDIO);

  lk.unlock();

  /* Play() outside lock — only at initial load, matching Kodi */
  if (call_play) {
    bool ok = media_call_bool(ctx, "Play", [&] { return ctx->media->Play(); });
    std::string rate_payload = starfish_json_build_play_rate(1000, true);
    (void)media_call_bool(ctx, "SetPlayRate", [&] {
      return ctx->media->SetPlayRate(rate_payload.c_str());
    });
    if (!ok)
      mp_err(ctx->log, "Starfish Play failed after load\n");
  }

  if (wake_video && video_wake.cb)
    video_wake.cb(video_wake.opaque);
  if (wake_audio && audio_wake.cb)
    audio_wake.cb(audio_wake.opaque);

  guard->active.fetch_sub(1, std::memory_order_acq_rel);
}

/* ------------------------------------------------------------------ */
/* public C API                                                        */
/* ------------------------------------------------------------------ */

extern "C" {

struct starfish_ctx *starfish_ctx_create(struct mp_log *log) {
  struct starfish_ctx *ctx = new starfish_ctx();
  ctx->callbacks = new callback_guard();
  ctx->callbacks->ctx.store(ctx, std::memory_order_release);
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

bool starfish_ctx_prime_media(void) {
  std::lock_guard<std::mutex> media_lock(g_media_init_lock);
  if (g_primed_media)
    return true;
  g_primed_media = create_media_instance(nullptr);
  return !!g_primed_media;
}

struct starfish_ctx *starfish_ctx_retain(struct starfish_ctx *ctx) {
  if (ctx)
    ctx->refs.fetch_add(1, std::memory_order_relaxed);
  return ctx;
}

void starfish_ctx_unref(struct starfish_ctx *ctx) {
  if (!ctx)
    return;
  if (ctx->refs.fetch_sub(1, std::memory_order_acq_rel) != 1)
    return;

  callback_guard *guard = ctx->callbacks;

  {
    std::lock_guard<std::mutex> lk(ctx->lock);
    ctx->stop = true;
  }
  ctx->cv.notify_all();
  if (ctx->worker.joinable())
    ctx->worker.join();

  if (guard) {
    guard->ctx.store(nullptr, std::memory_order_release);
    while (guard->active.load(std::memory_order_acquire) > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

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

struct starfish_ctx *starfish_ctx_from_hwdec(struct mp_hwdec_ctx *hwctx) {
  return hwctx ? (struct starfish_ctx *)hwctx->conversion_config : nullptr;
}

bool starfish_ctx_set_current(struct starfish_ctx *ctx) {
  std::lock_guard<std::mutex> lk(g_current_lock);
  if (ctx == g_current_ctx)
    return true;
  starfish_ctx_retain(ctx);
  starfish_ctx_unref(g_current_ctx);
  g_current_ctx = ctx;
  return true;
}

struct starfish_ctx *starfish_ctx_get_current(void) {
  std::lock_guard<std::mutex> lk(g_current_lock);
  return starfish_ctx_retain(g_current_ctx);
}

void starfish_ctx_set_wakeup_cb(struct starfish_ctx *ctx,
                                enum starfish_stream_type stream,
                                starfish_wakeup_cb cb, void *opaque) {
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (stream == STARFISH_STREAM_VIDEO) {
    ctx->video_wakeup = cb;
    ctx->video_wakeup_opaque = opaque;
  } else {
    ctx->audio_wakeup = cb;
    ctx->audio_wakeup_opaque = opaque;
  }
}

void starfish_ctx_set_audio_prime_cb(struct starfish_ctx *ctx,
                                     starfish_audio_prime_cb cb,
                                     void *opaque) {
  std::lock_guard<std::mutex> lk(ctx->lock);
  ctx->audio_prime = cb;
  ctx->audio_prime_opaque = opaque;
}

bool starfish_ctx_set_window_id(struct starfish_ctx *ctx,
                                const char *window_id) {
  {
    std::lock_guard<std::mutex> lk(ctx->lock);
    if (ctx->state == pipeline_state::LOADING || is_loaded_state(ctx->state))
      return false;
    ctx->window_id = window_id ? window_id : "";
  }
  ctx->cv.notify_all();
  return true;
}

bool starfish_ctx_set_numeric_window_id(struct starfish_ctx *ctx, int64_t wid) {
  char buf[32];
  if (wid <= 0)
    return false;
  snprintf(buf, sizeof(buf), "%" PRId64, wid);
  return starfish_ctx_set_window_id(ctx, buf);
}

bool starfish_ctx_set_video_geometry(struct starfish_ctx *ctx, int width,
                                     int height, double fps) {
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (is_loaded_state(ctx->state))
    return false;
  ctx->width = width;
  ctx->height = height;
  if (fps > 0) {
    AVRational r = av_d2q(fps, 1000000);
    ctx->fps = av_q2d(r);
    ctx->fps_num = r.num;
    ctx->fps_den = r.den;
  }
  return true;
}

bool starfish_ctx_set_display_window(struct starfish_ctx *ctx, int src_x,
                                     int src_y, int src_w, int src_h, int dst_x,
                                     int dst_y, int dst_w, int dst_h) {
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (!ctx->window_id.empty())
    return true;
  if (!ensure_acb(ctx))
    return false;
  int display_ret = AcbAPI_setDisplayWindow(ctx->acb_id, dst_x, dst_y, dst_w,
                                            dst_h, false, &ctx->acb_task_id);
  int custom_ret = AcbAPI_setCustomDisplayWindow(ctx->acb_id, src_x, src_y,
                                                 src_w, src_h, dst_x, dst_y,
                                                 dst_w, dst_h, false,
                                                 &ctx->acb_task_id);
  mp_info(ctx->log,
          "Starfish ACB window src=%d,%d %dx%d dst=%d,%d %dx%d ret display=%d custom=%d\n",
          src_x, src_y, src_w, src_h, dst_x, dst_y, dst_w, dst_h, display_ret,
          custom_ret);
  return display_ret == 0 || custom_ret == 0 || display_ret == 1 ||
         custom_ret == 1;
}

static void wait_for_callbacks_idle(callback_guard *guard) {
  if (!guard)
    return;
  while (guard->active.load(std::memory_order_acquire) > 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// Tear down a previous Starfish session (if any) so a fresh configure/Load
// cycle can run on the same long-lived ctx. Caller must NOT hold ctx->lock.
static void starfish_ctx_session_reset(struct starfish_ctx *ctx) {
  std::unique_lock<std::mutex> lk(ctx->lock);
  while (ctx->resetting && !ctx->stop)
    ctx->cv.wait(lk);
  if (ctx->stop)
    return;

  const bool had_session_state = ctx->state != pipeline_state::IDLE;
  const bool had_media = !!ctx->media;
  if (!had_session_state && !had_media)
    return;

  ctx->resetting = true;
  ctx->play_requested = false;
  ctx->flush_requested = false;
  ctx->eos_pending = false;
  clear_queued_packets_locked(ctx);
  lk.unlock();
  wait_for_callbacks_idle(ctx->callbacks);
  lk.lock();
  while (ctx->media_calls > 0 && !ctx->stop)
    ctx->cv.wait(lk);

  const bool need_unload = is_loaded_state(ctx->state) ||
                            ctx->state == pipeline_state::LOADING ||
                            ctx->state == pipeline_state::FAILED;

  if (need_unload && ctx->media) {
    lk.unlock();
    media_call_bool(ctx, "Unload", [&] { return ctx->media->Unload(); });
    lk.lock();

    // UNLOADCOMPLETED is delivered on Starfish's callback thread and
    // resets state to IDLE under ctx->lock; wait for it.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (ctx->state != pipeline_state::IDLE && !ctx->stop) {
      if (ctx->cv.wait_until(lk, deadline) == std::cv_status::timeout)
        break;
    }
    if (ctx->state != pipeline_state::IDLE) {
      mp_warn(ctx->log,
              "Starfish session reset: timed out waiting for "
              "UNLOADCOMPLETED, forcing IDLE\n");
      ctx->state = pipeline_state::IDLE;
    }
  } else {
    ctx->state = pipeline_state::IDLE;
  }

  lk.unlock();
  wait_for_callbacks_idle(ctx->callbacks);
  lk.lock();

  // Wipe per-session state so configure_* can repopulate.
  // Audio config is set once by AO init and must survive resets.
  ctx->video_codec.clear();
  ctx->ended = false;
  ctx->eos_pushed = false;
  ctx->eos_pending = false;
  ctx->started = false;
  ctx->need_segment = false;
  ctx->pending_seek_target = false;
  ctx->seek_target_valid = false;
  ctx->min_ready_pts_ns = INT64_MIN;
  ctx->fed_video_pts_ns = INT64_MIN;
  ctx->fed_audio_pts_ns = INT64_MIN;
  ctx->pts_offset_valid = false;
  ctx->pts_offset_ns = 0;
  ctx->ready_frames.clear();
  clear_queued_packets_locked(ctx);
  ctx->media.reset();
  ctx->resetting = false;
  ctx->cv.notify_all();
}

bool starfish_ctx_unload(struct starfish_ctx *ctx) {
  if (!ctx)
    return true;
  starfish_ctx_session_reset(ctx);
  return true;
}

bool starfish_ctx_configure_video(struct starfish_ctx *ctx,
                                  const struct mp_codec_params *codec) {
  const char *name =
      video_codec_name((enum AVCodecID)mp_codec_to_av_codec_id(codec->codec));
  if (!name)
    return false;

  // If we still hold a previous session's pipeline, unload it now so the
  // new file can configure from a clean IDLE state.
  starfish_ctx_session_reset(ctx);

  const AVDOVIDecoderConfigurationRecord *dovi = find_dovi_config(codec);
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (is_loaded_state(ctx->state))
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
  ctx->effective_dovi = resolve_effective_dovi(ctx);

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

  mp_info(ctx->log,
          "Configured Starfish video: codec=%s trc=%d dovi=%d/%d profile=%u "
          "policy=%s\n",
          ctx->video_codec.c_str(), ctx->video_color.transfer, ctx->source_dovi,
          ctx->effective_dovi, ctx->dv_profile,
          dovi_policy_name(ctx->dovi_mode));
  return true;
}

bool starfish_ctx_configure_audio_passthrough(struct starfish_ctx *ctx,
                                              int format, int samplerate,
                                              const struct mp_chmap *channels) {
  const char *name = audio_codec_name_from_format(format);
  if (!name)
    return false;
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (is_loaded_state(ctx->state))
    return false;
  ctx->audio_codec = name;
  ctx->need_audio = true;
  return true;
}

bool starfish_ctx_configure_audio_aac(struct starfish_ctx *ctx, int channels,
                                      int samplerate, int profile, bool raw) {
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (is_loaded_state(ctx->state)) {
    const bool same_config = ctx->need_audio && ctx->audio_codec == "AAC" &&
                             ctx->audio_channels == channels &&
                             ctx->audio_samplerate == samplerate &&
                             ctx->audio_profile == profile &&
                             ctx->audio_raw == raw;
    if (!same_config) {
      mp_warn(ctx->log,
              "Rejecting Starfish AAC reconfigure while loaded: "
              "current=%s/%d/%d/%d/%d requested=AAC/%d/%d/%d/%d\n",
              ctx->audio_codec.c_str(), ctx->audio_channels,
              ctx->audio_samplerate, ctx->audio_profile, ctx->audio_raw,
              channels, samplerate, profile, raw);
      return false;
    }
    ctx->audio_queue.clear();
    ctx->audio_queue_bytes = 0;
    ctx->fed_audio_pts_ns = INT64_MIN;
    ctx->audio_bufferfull_logs = 0;
    ctx->cv.notify_all();
    mp_info(ctx->log, "Reusing loaded Starfish AAC audio configuration\n");
    return true;
  }
  ctx->audio_codec = "AAC";
  ctx->audio_channels = channels;
  ctx->audio_samplerate = samplerate;
  ctx->audio_profile = profile;
  ctx->audio_raw = raw;
  ctx->need_audio = true;
  return true;
}

/* ------------------------------------------------------------------ */
/* feed — called directly from mpv decoder/audio threads               */
/* ------------------------------------------------------------------ */

int starfish_ctx_feed_video(struct starfish_ctx *ctx, const void *data,
                            size_t size, double pts, bool keyframe) {
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (ctx->resetting)
    return STARFISH_FEED_AGAIN;
  if (ctx->state == pipeline_state::FAILED)
    return STARFISH_FEED_ERROR;
  if (!have_load_config(ctx))
    return STARFISH_FEED_ERROR;
  if (ctx->flush_requested)
    return STARFISH_FEED_AGAIN;
  if (ctx->video_queue_bytes + size > VIDEO_QUEUE_LIMIT)
    return STARFISH_FEED_AGAIN;

  queued_packet packet;
  packet.data = std::make_shared<std::vector<uint8_t>>(
      (const uint8_t *)data, (const uint8_t *)data + size);
  packet.pts_ns = pts == MP_NOPTS_VALUE ? 0 : (int64_t)(pts * 1e9);
  packet.keyframe = keyframe;
  ctx->video_queue_bytes += size;
  ctx->video_queue.push_back(std::move(packet));
  ctx->cv.notify_all();
  return STARFISH_FEED_OK;
}

int starfish_ctx_feed_audio(struct starfish_ctx *ctx, const void *data,
                            size_t size, int64_t pts_ns) {
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (ctx->resetting)
    return STARFISH_FEED_AGAIN;
  if (ctx->state == pipeline_state::FAILED)
    return STARFISH_FEED_ERROR;
  if (ctx->flush_requested)
    return STARFISH_FEED_AGAIN;
  if (ctx->audio_queue_bytes + size > AUDIO_QUEUE_LIMIT)
    return STARFISH_FEED_AGAIN;

  queued_packet packet;
  packet.data = std::make_shared<std::vector<uint8_t>>(
      (const uint8_t *)data, (const uint8_t *)data + size);
  packet.pts_ns = pts_ns;
  ctx->audio_queue_bytes += size;
  ctx->audio_queue.push_back(std::move(packet));
  ctx->cv.notify_all();
  return STARFISH_FEED_OK;
}

/* ------------------------------------------------------------------ */
/* frame readback                                                      */
/* ------------------------------------------------------------------ */

bool starfish_ctx_pop_video_frame(struct starfish_ctx *ctx,
                                  struct starfish_video_frame *frame) {
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (ctx->ready_frames.empty())
    return false;
  *frame = ctx->ready_frames.front();
  ctx->ready_frames.pop_front();
  return true;
}

/* ------------------------------------------------------------------ */
/* playback control                                                    */
/* ------------------------------------------------------------------ */

bool starfish_ctx_resume(struct starfish_ctx *ctx) {
  {
    std::lock_guard<std::mutex> lk(ctx->lock);
    ctx->play_requested = true;
  }
  ctx->cv.notify_all();
  return true;
}

bool starfish_ctx_pause(struct starfish_ctx *ctx) {
  {
    std::lock_guard<std::mutex> lk(ctx->lock);
    ctx->play_requested = false;
  }
  ctx->cv.notify_all();
  return true;
}

bool starfish_ctx_set_seek_target(struct starfish_ctx *ctx, double pts) {
  if (pts == MP_NOPTS_VALUE)
    return true;
  std::lock_guard<std::mutex> lk(ctx->lock);
  ctx->pending_seek_target = true;
  ctx->pending_seek_target_ns = (int64_t)(pts * 1e9);
  ctx->seek_target_valid = true;
  ctx->seek_target_ns = ctx->pending_seek_target_ns;
  return true;
}

bool starfish_ctx_flush(struct starfish_ctx *ctx, double pts) {
  {
    std::lock_guard<std::mutex> lk(ctx->lock);
    ctx->flush_requested = true;
    if (pts != MP_NOPTS_VALUE) {
      ctx->flush_pts_ns = (int64_t)(pts * 1e9);
      ctx->pending_seek_target = true;
      ctx->pending_seek_target_ns = ctx->flush_pts_ns;
      ctx->seek_target_valid = true;
      ctx->seek_target_ns = ctx->flush_pts_ns;
      mp_info(ctx->log, "Starfish flush pts set: %" PRId64 "\n",
              ctx->seek_target_ns);
    }
  }
  ctx->cv.notify_all();
  return true;
}

bool starfish_ctx_get_seek_target_ns(struct starfish_ctx *ctx,
                                     int64_t *pts_ns) {
  if (!ctx || !pts_ns)
    return false;
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (!ctx->seek_target_valid)
    return false;
  *pts_ns = ctx->seek_target_ns;
  return true;
}

bool starfish_ctx_get_audio_reset_target_ns(struct starfish_ctx *ctx,
                                            int64_t *pts_ns,
                                            bool *needs_segment_prime) {
  if (!ctx || !pts_ns || !needs_segment_prime)
    return false;
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (!is_loaded_state(ctx->state))
    return false;
  *needs_segment_prime =
      ctx->flush_requested || ctx->need_segment || ctx->pending_seek_target;
  if (*needs_segment_prime && ctx->seek_target_valid)
    *pts_ns = ctx->seek_target_ns;
  else
    *pts_ns = ctx->current_pts_ns;
  return true;
}

bool starfish_ctx_push_eos(struct starfish_ctx *ctx) {
  {
    std::lock_guard<std::mutex> lk(ctx->lock);
    if (ctx->eos_pushed)
      return true;
    ctx->eos_pending = true;
    ctx->ended = false;
  }
  ctx->cv.notify_all();
  return true;
}

bool starfish_ctx_has_ended(struct starfish_ctx *ctx) {
  std::lock_guard<std::mutex> lk(ctx->lock);
  return ctx->ended && ctx->ready_frames.empty();
}

int starfish_ctx_get_video_width(struct starfish_ctx *ctx) {
  std::lock_guard<std::mutex> lk(ctx->lock);
  return ctx->width;
}

int starfish_ctx_get_video_height(struct starfish_ctx *ctx) {
  std::lock_guard<std::mutex> lk(ctx->lock);
  return ctx->height;
}

double starfish_ctx_get_video_fps(struct starfish_ctx *ctx) {
  std::lock_guard<std::mutex> lk(ctx->lock);
  return ctx->fps;
}

double starfish_ctx_get_current_pts(struct starfish_ctx *ctx) {
  std::lock_guard<std::mutex> lk(ctx->lock);
  return ctx->current_pts_ns / 1000000000.0;
}

int starfish_ctx_get_dovi_profile(struct starfish_ctx *ctx) {
  std::lock_guard<std::mutex> lk(ctx->lock);
  return ctx->dv_profile;
}

} // extern "C"
