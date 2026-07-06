/*
 * starfish_ctx — Starfish session: state machine, packet queue, worker
 * thread, clock sampler.
 *
 * Design rule: Starfish video is master. The session samples
 * StarfishMediaAPIs::getCurrentPlaytime() and exports it via
 * starfish_ctx_get_video_clock(); mpv's audio resampler follows that clock
 * through VOCTRL_GET_EXTERNAL_VIDEO_CLOCK and --video-sync=display-resample.
 *
 * No path in this file injects an external clock back into Starfish. Audio
 * (when ao_starfish is in use) flows in the same direction as video: into
 * Starfish, never out as a clock signal.
 */

#include "starfish_ctx.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <inttypes.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
#include "osdep/timer.h"
#include "video/hwdec.h"
#undef _Atomic
}

#include "starfish_backend.h"
#include "starfish_json.h"

namespace {

constexpr size_t VIDEO_QUEUE_LIMIT = 32 * 1024 * 1024;
constexpr size_t AUDIO_QUEUE_LIMIT = 2 * 1024 * 1024;
// Kodi's webOS pipeline caps Starfish feed-ahead at 1.6s. Keeping the same
// order of magnitude avoids multi-second stale queues after seek/resume.
constexpr int64_t MAX_DECODER_ACCEPT_AHEAD_NS = 1600LL * 1000 * 1000;
constexpr int64_t MAX_FEED_AHEAD_NS = 1600LL * 1000 * 1000;
constexpr int64_t PCM_DECODE_PREROLL_NS = 0;
constexpr int64_t STALE_READY_TOLERANCE_NS = 1000LL * 1000;
constexpr int64_t READY_CEILING_SLACK_NS = 5LL * 1000 * 1000 * 1000;
constexpr int64_t CLOCK_SAMPLE_PERIOD_NS = 20LL * 1000 * 1000;
constexpr int64_t CLOCK_SAMPLE_SLOW_NS = 50LL * 1000 * 1000;
constexpr int64_t CLOCK_BACKWARD_TOLERANCE_NS = 100LL * 1000 * 1000;
constexpr int64_t CLOCK_FED_VIDEO_SLACK_NS = 500LL * 1000 * 1000;
constexpr int64_t CLOCK_FRESHNESS_NS = 250LL * 1000 * 1000;
constexpr int64_t CLOCK_EXPORT_STABLE_WINDOW_NS = 250LL * 1000 * 1000;
constexpr double CLOCK_EXPORT_MIN_RATE = 0.80;
constexpr double CLOCK_EXPORT_MAX_RATE = 1.20;
constexpr int64_t PENDING_PLAYING_CLOCK_TOLERANCE_NS = 5LL * 1000 * 1000 * 1000;
constexpr int64_t WORKER_STATUS_PERIOD_NS = 1000LL * 1000 * 1000;
constexpr int64_t FAILURE_WAKE_PERIOD_NS = 250LL * 1000 * 1000;
constexpr int64_t BUFFERLOW_STALL_TIMEOUT_NS = 5000LL * 1000 * 1000;
constexpr auto SEGMENT_READY_PLAY_WATCHDOG = std::chrono::milliseconds(1500);
constexpr auto WORKER_IDLE_WAIT = std::chrono::milliseconds(20);
constexpr auto BUFFERFULL_BACKOFF = std::chrono::milliseconds(25);

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

struct wakeup_client {
  std::mutex lock;
  starfish_wakeup_cb cb = nullptr;
  void *opaque = nullptr;
};

struct audio_prime_client {
  std::mutex lock;
  starfish_audio_prime_cb cb = nullptr;
  void *opaque = nullptr;
};

const char *get_app_id() {
  const char *app_id = getenv("APPID");
  return app_id && app_id[0] ? app_id : "mpv";
}

bool env_wants_audio_hint() {
  const char *hint = getenv("STARFISH_AUDIO_HINT");
  return hint && hint[0] && strcmp(hint, "0") != 0;
}

// Which Starfish audio ES the app wants. "pcm" routes decoded audio as raw
// PCM (codec token "PCM", parsed by libpf's setPCMinfo); anything else
// (default) keeps the legacy AAC encode path.
bool env_wants_pcm_audio() {
  const char *codec = getenv("STARFISH_AUDIO_CODEC");
  return codec && (strcmp(codec, "pcm") == 0 || strcmp(codec, "PCM") == 0);
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
  case dovi_policy::AUTO: return "auto";
  case dovi_policy::PASSTHROUGH: return "passthrough";
  case dovi_policy::P7_FALLBACK: return "p7-fallback";
  case dovi_policy::HDR10: return "hdr10";
  }
  return "auto";
}

const char *video_codec_name(enum AVCodecID codec) {
  switch (codec) {
  case AV_CODEC_ID_VP8: return "VP8";
  case AV_CODEC_ID_VP9: return "VP9";
  case AV_CODEC_ID_H264:
  case AV_CODEC_ID_AVS:
  case AV_CODEC_ID_CAVS: return "H264";
  case AV_CODEC_ID_HEVC: return "H265";
  case AV_CODEC_ID_AV1: return "AV1";
  default: return nullptr;
  }
}

const char *audio_codec_name_from_format(int format) {
  switch (format) {
  case AF_FORMAT_S_AAC: return "AAC";
  case AF_FORMAT_S_AC3: return "AC3";
  case AF_FORMAT_S_EAC3: return "AC3 PLUS";
  case AF_FORMAT_S_DTS:
  case AF_FORMAT_S_DTSHD: return "DTS";
  case AF_FORMAT_S_MP3: return "MP3";
  case AF_FORMAT_S_TRUEHD: return "TRUEHD";
  default: return nullptr;
  }
}

} // namespace

struct starfish_ctx {
  std::atomic<int> refs{1};
  struct mp_log *log = nullptr;
  std::mutex lock;
  std::condition_variable cv;
  std::thread worker;
  bool stop = false;

  sf_backend *backend = nullptr;
  sf_backend_acb *acb = nullptr;

  /* window */
  std::string window_id;

  /* video config */
  std::string video_codec;
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
  /* PCM-only (audio_codec == "PCM") */
  int audio_bits_per_sample = 0;
  std::string audio_pcm_format;
  std::string audio_pcm_layout;
  int64_t audio_decode_preroll_ns = 0;

  /* DOVI / HDR */
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
  bool eos_pushed = false;
  bool eos_pending = false;
  bool ended = false;
  bool started = false;

  /* seek */
  bool pending_seek_target = false;
  int64_t pending_seek_target_ns = 0;
  bool seek_target_valid = false;
  int64_t seek_target_ns = 0;

  /* segment timeline */
  int64_t min_ready_pts_ns = INT64_MIN;
  int64_t current_pts_ns = 0;

  /* segment first-frame deferral */
  bool pending_segment_ready_frame = false;
  int64_t pending_segment_ready_pts_ns = INT64_MIN;
  bool pending_segment_ready_have_pts = false;
  bool pending_segment_ready_saw_play = false;
  bool pending_segment_ready_await_play = false;
  bool first_segment_after_load = false;
  std::chrono::steady_clock::time_point pending_segment_ready_watchdog =
      std::chrono::steady_clock::time_point::min();
  bool play_after_preroll_pending = false;
  int64_t play_after_preroll_target_ns = INT64_MIN;

  /* fed packet tracking */
  int64_t fed_video_pts_ns = INT64_MIN;
  int64_t fed_audio_pts_ns = INT64_MIN;
  int video_bufferfull_logs = 0;
  int audio_bufferfull_logs = 0;

  /* worker diagnostics */
  bool feed_inflight = false;
  enum starfish_stream_type feed_inflight_stream = STARFISH_STREAM_VIDEO;
  int64_t feed_inflight_start_ns = 0;
  int64_t feed_inflight_media_pts_ns = INT64_MIN;
  int64_t feed_inflight_sdk_pts_ns = INT64_MIN;
  size_t feed_inflight_size = 0;
  enum sf_backend_feed_result last_feed_result = SF_BACKEND_FEED_ERROR;
  enum starfish_stream_type last_feed_stream = STARFISH_STREAM_VIDEO;
  int64_t last_feed_done_ns = 0;
  int64_t last_feed_media_pts_ns = INT64_MIN;
  int64_t last_feed_sdk_pts_ns = INT64_MIN;
  int64_t last_worker_status_log_ns = 0;
  int64_t last_decoder_backpressure_log_ns = 0;
  int64_t last_failure_wake_ns = 0;
  int64_t last_clock_jump_log_ns = 0;
  int clock_jump_suppressed = 0;
  int64_t last_clock_reject_log_ns = 0;
  int clock_reject_suppressed = 0;
  int64_t video_backpressure_pts_ns = INT64_MIN;
  std::string failure_reason;
  bool bufferlow_active = false;
  int64_t bufferlow_start_ns = 0;
  int64_t bufferlow_last_progress_pts_ns = INT64_MIN;
  int64_t bufferlow_last_progress_host_ns = 0;

  /* packet queues */
  std::deque<queued_packet> video_queue;
  std::deque<queued_packet> audio_queue;
  size_t video_queue_bytes = 0;
  size_t audio_queue_bytes = 0;

  /* ready frames for VO */
  std::deque<starfish_video_frame> ready_frames;
  int64_t video_clock_base_pts_ns = INT64_MIN;
  int64_t video_clock_base_host_ns = 0;

  /* sampled Starfish clock (for VOCTRL_GET_EXTERNAL_VIDEO_CLOCK) */
  bool clock_sample_valid = false;
  double clock_sample_pts = 0.0;
  /* Host-time anchor for clock_sample_pts: estimate of when the frame that
   * getCurrentPlaytime currently reports actually flipped, NOT the time of the
   * latest poll. Consumers project pts + (now - anchor). */
  int64_t clock_sample_host_ns = 0;
  /* Host time of the most recent accepted poll, used to bracket the flip
   * instant when the reported pts advances. */
  int64_t clock_last_poll_host_ns = 0;
  bool clock_export_ready = false;
  int64_t clock_stability_probe_pts_ns = INT64_MIN;
  int64_t clock_stability_probe_host_ns = 0;
  int64_t last_clock_attempt_ns = 0;
  bool log_next_clock_sample = true;

  /* callbacks */
  wakeup_client video_wakeup;
  wakeup_client audio_wakeup;
  audio_prime_client audio_prime;
};

/* ----- global current ctx ------------------------------------------- */

static std::mutex g_current_lock;
static starfish_ctx *g_current_ctx;

/* starfish_overlay_set_present_cb and starfish_exported_set_crop_cb live in
 * vo_starfish.c since they only feed VO-local OSD/crop state. */

/* ----- wakeup / prime client helpers -------------------------------- */

static void call_wakeup(wakeup_client *c) {
  starfish_wakeup_cb cb = nullptr;
  void *opaque = nullptr;
  {
    std::lock_guard<std::mutex> lk(c->lock);
    cb = c->cb;
    opaque = c->opaque;
  }
  if (cb)
    cb(opaque);
}

static bool call_audio_prime(audio_prime_client *c, int64_t pts_ns) {
  starfish_audio_prime_cb cb = nullptr;
  void *opaque = nullptr;
  {
    std::lock_guard<std::mutex> lk(c->lock);
    cb = c->cb;
    opaque = c->opaque;
  }
  return cb ? cb(opaque, pts_ns) : false;
}

static void set_wakeup(wakeup_client *c, starfish_wakeup_cb cb, void *opaque) {
  std::lock_guard<std::mutex> lk(c->lock);
  c->cb = cb;
  c->opaque = opaque;
}

static void set_audio_prime(audio_prime_client *c, starfish_audio_prime_cb cb,
                            void *opaque) {
  std::lock_guard<std::mutex> lk(c->lock);
  c->cb = cb;
  c->opaque = opaque;
}

static void wake_stream(starfish_ctx *ctx, enum starfish_stream_type stream) {
  call_wakeup(stream == STARFISH_STREAM_VIDEO ? &ctx->video_wakeup
                                              : &ctx->audio_wakeup);
}

static void wake_all(starfish_ctx *ctx) {
  wake_stream(ctx, STARFISH_STREAM_VIDEO);
  wake_stream(ctx, STARFISH_STREAM_AUDIO);
}

static const char *stream_name(enum starfish_stream_type stream) {
  return stream == STARFISH_STREAM_AUDIO ? "audio" : "video";
}

static const char *pipeline_state_name(pipeline_state state) {
  switch (state) {
  case pipeline_state::IDLE: return "idle";
  case pipeline_state::LOADING: return "loading";
  case pipeline_state::LOADED: return "loaded";
  case pipeline_state::PLAYING: return "playing";
  case pipeline_state::PAUSED: return "paused";
  case pipeline_state::FAILED: return "failed";
  }
  return "unknown";
}

static const char *feed_result_name(enum sf_backend_feed_result result) {
  switch (result) {
  case SF_BACKEND_FEED_ERROR: return "error";
  case SF_BACKEND_FEED_OK: return "ok";
  case SF_BACKEND_FEED_BUFFER_FULL: return "bufferfull";
  case SF_BACKEND_FEED_RETRY: return "retry";
  }
  return "unknown";
}

static double ns_to_sec_or_neg(int64_t ns) {
  return ns == INT64_MIN ? -1.0 : (double)ns / 1e9;
}

static void log_worker_status_locked(starfish_ctx *ctx, const char *reason) {
  const int64_t now = mp_time_ns();
  const double inflight_age_ms =
      ctx->feed_inflight && ctx->feed_inflight_start_ns > 0
          ? (now - ctx->feed_inflight_start_ns) / 1e6
          : -1.0;
  const double last_feed_age_ms =
      ctx->last_feed_done_ns > 0 ? (now - ctx->last_feed_done_ns) / 1e6 : -1.0;
  const double clock_age_ms =
      ctx->clock_sample_valid && ctx->clock_sample_host_ns > 0
          ? (now - ctx->clock_sample_host_ns) / 1e6
          : -1.0;
  const double bufferlow_age_ms =
      ctx->bufferlow_active && ctx->bufferlow_start_ns > 0
          ? (now - ctx->bufferlow_start_ns) / 1e6
          : -1.0;
  const double bufferlow_progress_age_ms =
      ctx->bufferlow_active && ctx->bufferlow_last_progress_host_ns > 0
          ? (now - ctx->bufferlow_last_progress_host_ns) / 1e6
          : -1.0;

  mp_info(ctx->log,
          "Starfish worker status reason=%s state=%s started=%d play=%d "
          "need_segment=%d flush=%d queues video=%.2fMB/%zu audio=%.2fMB/%zu "
          "current=%.3f fed_v=%.3f fed_a=%.3f clock_valid=%d clock=%.3f "
          "clock_age=%.1fms inflight=%d stream=%s pts=%.3f sdk_pts=%.3f "
          "size=%zu age=%.1fms last_feed=%s stream=%s pts=%.3f sdk_pts=%.3f "
          "age=%.1fms backpressure=%.3f bufferlow=%d age=%.1fms "
          "progress_pts=%.3f progress_age=%.1fms\n",
          reason ? reason : "unknown", pipeline_state_name(ctx->state),
          ctx->started, ctx->play_requested, ctx->need_segment,
          ctx->flush_requested, ctx->video_queue_bytes / 1024.0 / 1024.0,
          ctx->video_queue.size(), ctx->audio_queue_bytes / 1024.0 / 1024.0,
          ctx->audio_queue.size(), ns_to_sec_or_neg(ctx->current_pts_ns),
          ns_to_sec_or_neg(ctx->fed_video_pts_ns),
          ns_to_sec_or_neg(ctx->fed_audio_pts_ns), ctx->clock_sample_valid,
          ctx->clock_sample_valid ? ctx->clock_sample_pts : -1.0,
          clock_age_ms, ctx->feed_inflight,
          stream_name(ctx->feed_inflight_stream),
          ns_to_sec_or_neg(ctx->feed_inflight_media_pts_ns),
          ns_to_sec_or_neg(ctx->feed_inflight_sdk_pts_ns),
          ctx->feed_inflight_size, inflight_age_ms,
          feed_result_name(ctx->last_feed_result),
          stream_name(ctx->last_feed_stream),
          ns_to_sec_or_neg(ctx->last_feed_media_pts_ns),
          ns_to_sec_or_neg(ctx->last_feed_sdk_pts_ns), last_feed_age_ms,
          ns_to_sec_or_neg(ctx->video_backpressure_pts_ns),
          ctx->bufferlow_active, bufferlow_age_ms,
          ns_to_sec_or_neg(ctx->bufferlow_last_progress_pts_ns),
          bufferlow_progress_age_ms);
}

static void maybe_log_worker_status_locked(starfish_ctx *ctx,
                                           const char *reason) {
  const int64_t now = mp_time_ns();
  if (ctx->last_worker_status_log_ns &&
      now - ctx->last_worker_status_log_ns < WORKER_STATUS_PERIOD_NS)
    return;
  ctx->last_worker_status_log_ns = now;
  log_worker_status_locked(ctx, reason);
}

static bool is_loaded_state(pipeline_state s);

static bool should_feed_packets_locked(starfish_ctx *ctx) {
  if (!is_loaded_state(ctx->state))
    return false;
  if (!ctx->play_requested &&
      (ctx->state == pipeline_state::PAUSED ||
       ctx->state == pipeline_state::LOADED))
    return false;
  return true;
}

static void mark_pipeline_failed_locked(starfish_ctx *ctx, const char *reason) {
  ctx->state = pipeline_state::FAILED;
  ctx->ended = true;
  ctx->failure_reason = reason ? reason : "unknown";
  ctx->last_failure_wake_ns = 0;
  ctx->video_backpressure_pts_ns = INT64_MIN;
}

/* ----- DOVI / HDR helpers ------------------------------------------- */

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

static bool resolve_effective_dovi(starfish_ctx *ctx) {
  if (!ctx->source_dovi)
    return false;
  switch (ctx->dovi_mode) {
  case dovi_policy::HDR10: return false;
  case dovi_policy::PASSTHROUGH: return true;
  case dovi_policy::AUTO:
  case dovi_policy::P7_FALLBACK:
    if (ctx->dv_profile == 7 && ctx->dv_el_present)
      mp_info(ctx->log,
              "Starfish: Profile 7 detected, allowing conversion to 8.1\n");
    return true;
  }
  return false;
}

static const char *hdr_type_name(const starfish_ctx *ctx) {
  switch (ctx->video_color.transfer) {
  case PL_COLOR_TRC_PQ: return "hdr10";
  case PL_COLOR_TRC_HLG: return "hlg";
  default: return "none";
  }
}

static bool hdr_sei_available(const starfish_ctx *ctx) {
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
  case PL_COLOR_SYSTEM_BT_601: return AVCOL_SPC_SMPTE170M;
  case PL_COLOR_SYSTEM_BT_709: return AVCOL_SPC_BT709;
  case PL_COLOR_SYSTEM_SMPTE_240M: return AVCOL_SPC_SMPTE240M;
  case PL_COLOR_SYSTEM_BT_2020_NC: return AVCOL_SPC_BT2020_NCL;
  case PL_COLOR_SYSTEM_BT_2020_C: return AVCOL_SPC_BT2020_CL;
  case PL_COLOR_SYSTEM_BT_2100_PQ:
  case PL_COLOR_SYSTEM_BT_2100_HLG: return AVCOL_SPC_ICTCP;
  case PL_COLOR_SYSTEM_YCGCO: return AVCOL_SPC_YCGCO;
  case PL_COLOR_SYSTEM_RGB: return AVCOL_SPC_RGB;
  default: return AVCOL_SPC_UNSPECIFIED;
  }
}

static enum AVColorRange color_levels_to_av(enum pl_color_levels levels) {
  switch (levels) {
  case PL_COLOR_LEVELS_LIMITED: return AVCOL_RANGE_MPEG;
  case PL_COLOR_LEVELS_FULL: return AVCOL_RANGE_JPEG;
  default: return AVCOL_RANGE_UNSPECIFIED;
  }
}

static enum AVColorPrimaries
color_primaries_to_av(enum pl_color_primaries prim) {
  switch (prim) {
  case PL_COLOR_PRIM_BT_601_525: return AVCOL_PRI_SMPTE170M;
  case PL_COLOR_PRIM_BT_601_625: return AVCOL_PRI_BT470BG;
  case PL_COLOR_PRIM_BT_709: return AVCOL_PRI_BT709;
  case PL_COLOR_PRIM_BT_470M: return AVCOL_PRI_BT470M;
  case PL_COLOR_PRIM_EBU_3213: return AVCOL_PRI_JEDEC_P22;
  case PL_COLOR_PRIM_BT_2020: return AVCOL_PRI_BT2020;
  case PL_COLOR_PRIM_CIE_1931: return AVCOL_PRI_SMPTE428;
  case PL_COLOR_PRIM_DCI_P3: return AVCOL_PRI_SMPTE431;
  case PL_COLOR_PRIM_DISPLAY_P3: return AVCOL_PRI_SMPTE432;
  case PL_COLOR_PRIM_FILM_C: return AVCOL_PRI_FILM;
  default: return AVCOL_PRI_UNSPECIFIED;
  }
}

static enum AVColorTransferCharacteristic
color_transfer_to_av(enum pl_color_transfer trc) {
  switch (trc) {
  case PL_COLOR_TRC_BT_1886: return AVCOL_TRC_BT709;
  case PL_COLOR_TRC_SRGB: return AVCOL_TRC_IEC61966_2_1;
  case PL_COLOR_TRC_LINEAR: return AVCOL_TRC_LINEAR;
  case PL_COLOR_TRC_GAMMA22: return AVCOL_TRC_GAMMA22;
  case PL_COLOR_TRC_GAMMA28: return AVCOL_TRC_GAMMA28;
  case PL_COLOR_TRC_ST428: return AVCOL_TRC_SMPTE428;
  case PL_COLOR_TRC_PQ: return AVCOL_TRC_SMPTE2084;
  case PL_COLOR_TRC_HLG: return AVCOL_TRC_ARIB_STD_B67;
  default: return AVCOL_TRC_UNSPECIFIED;
  }
}

static void apply_hdr_info(starfish_ctx *ctx) {
  const char *hdr_type = hdr_type_name(ctx);
  if (strcmp(hdr_type, "none") == 0 || !hdr_sei_available(ctx))
    return;

  const struct pl_hdr_metadata *hdr = &ctx->video_color.hdr;
  struct starfish_json_hdr_info_params p = {
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

  std::string payload = starfish_json_build_hdr_info(&p);
  mp_info(ctx->log, "Starfish setHdrInfo payload: %s\n", payload.c_str());
  if (!sf_backend_set_hdr_info(ctx->backend, payload.c_str()))
    mp_warn(ctx->log, "Starfish setHdrInfo failed\n");
}

/* ----- session lifecycle ------------------------------------------- */

static bool have_load_config(starfish_ctx *ctx) {
  return !ctx->video_codec.empty() && ctx->width > 0 && ctx->height > 0 &&
         ctx->fps_num > 0 && ctx->fps_den > 0;
}

static bool is_loaded_state(pipeline_state s) {
  return s == pipeline_state::LOADED || s == pipeline_state::PLAYING ||
         s == pipeline_state::PAUSED;
}

static bool can_load(starfish_ctx *ctx) {
  if (ctx->state != pipeline_state::IDLE)
    return false;
  if (!have_load_config(ctx))
    return false;
  if (ctx->video_queue.empty())
    return false;
  if (ctx->need_audio && ctx->audio_queue.empty())
    return false;
  return true;
}

static int64_t choose_segment_start_ns(starfish_ctx *ctx,
                                       int64_t packet_pts_ns) {
  if (ctx->pending_seek_target && packet_pts_ns <= ctx->pending_seek_target_ns)
    return ctx->pending_seek_target_ns;
  return packet_pts_ns;
}

static int64_t decode_start_for_segment_ns(starfish_ctx *ctx,
                                           int64_t segment_pts_ns) {
  if (segment_pts_ns > 0 && ctx->audio_decode_preroll_ns > 0)
    return std::max<int64_t>(0, segment_pts_ns - ctx->audio_decode_preroll_ns);
  return segment_pts_ns;
}

static void prepare_segment_timeline_locked(starfish_ctx *ctx,
                                            int64_t start_pts_ns,
                                            const char *reason) {
  ctx->started = false;
  ctx->current_pts_ns = start_pts_ns == INT64_MIN ? 0 : start_pts_ns;
  ctx->video_clock_base_pts_ns = INT64_MIN;
  ctx->video_clock_base_host_ns = 0;
  ctx->min_ready_pts_ns = start_pts_ns;
  ctx->fed_video_pts_ns = INT64_MIN;
  ctx->fed_audio_pts_ns = INT64_MIN;
  ctx->pending_segment_ready_frame = false;
  ctx->pending_segment_ready_pts_ns = INT64_MIN;
  ctx->pending_segment_ready_have_pts = false;
  ctx->pending_segment_ready_saw_play = false;
  ctx->pending_segment_ready_await_play = false;
  ctx->pending_segment_ready_watchdog =
      std::chrono::steady_clock::time_point::min();
  ctx->play_after_preroll_pending = false;
  ctx->play_after_preroll_target_ns = INT64_MIN;
  ctx->ready_frames.clear();
  ctx->clock_sample_valid = false;
  ctx->clock_export_ready = false;
  ctx->clock_stability_probe_pts_ns = INT64_MIN;
  ctx->clock_stability_probe_host_ns = 0;
  ctx->last_clock_attempt_ns = 0;
  ctx->log_next_clock_sample = true;
  ctx->last_decoder_backpressure_log_ns = 0;
  ctx->last_clock_jump_log_ns = 0;
  ctx->clock_jump_suppressed = 0;
  ctx->last_clock_reject_log_ns = 0;
  ctx->clock_reject_suppressed = 0;
  ctx->video_backpressure_pts_ns = INT64_MIN;
  ctx->bufferlow_active = false;
  ctx->bufferlow_start_ns = 0;
  ctx->bufferlow_last_progress_pts_ns = INT64_MIN;
  ctx->bufferlow_last_progress_host_ns = 0;
  mp_info(ctx->log, "Starfish segment timeline reset reason=%s target=%.3f\n",
          reason ? reason : "unknown",
          start_pts_ns == INT64_MIN ? -1.0 : (double)start_pts_ns / 1e9);
}

static void clear_queues_locked(starfish_ctx *ctx) {
  ctx->video_queue.clear();
  ctx->audio_queue.clear();
  ctx->video_queue_bytes = 0;
  ctx->audio_queue_bytes = 0;
  prepare_segment_timeline_locked(ctx, INT64_MIN, "clear-queues");
  ctx->video_bufferfull_logs = 0;
  ctx->audio_bufferfull_logs = 0;
}

static void backend_event_cb(void *opaque, enum sf_backend_event_type type,
                             int64_t num_value, const char *str_value);

static bool ensure_backend(starfish_ctx *ctx) {
  if (ctx->backend)
    return true;
  sf_backend_callbacks cb = { backend_event_cb, ctx };
  ctx->backend = sf_backend_create(ctx->log, &cb);
  return ctx->backend != nullptr;
}

static bool ensure_acb(starfish_ctx *ctx) {
  if (!ctx->window_id.empty() || ctx->acb)
    return true;
  ctx->acb = sf_backend_acb_create(ctx->log, get_app_id());
  return ctx->acb != nullptr;
}

static bool try_start_load(starfish_ctx *ctx,
                           std::unique_lock<std::mutex> &lk) {
  if (!can_load(ctx))
    return false;
  if (ctx->window_id.empty() && !ensure_acb(ctx))
    return false;

  const int64_t first_video_pts_ns = ctx->video_queue.empty()
                                     ? 0
                                     : ctx->video_queue.front().pts_ns;
  const int64_t load_pts_ns = choose_segment_start_ns(ctx, first_video_pts_ns);
  const int64_t decode_pts_ns = decode_start_for_segment_ns(ctx, load_pts_ns);

  struct starfish_json_load_params p = {
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
      .pts_to_decode_ns = decode_pts_ns,
      .need_audio = ctx->need_audio,
      .audio_sync = ctx->need_audio,
      .audio_bits_per_sample = ctx->audio_bits_per_sample,
      .audio_pcm_format =
          ctx->audio_pcm_format.empty() ? nullptr : ctx->audio_pcm_format.c_str(),
      .audio_pcm_layout =
          ctx->audio_pcm_layout.empty() ? nullptr : ctx->audio_pcm_layout.c_str(),
  };

  std::string payload = starfish_json_build_load(&p);
  ctx->state = pipeline_state::LOADING;
  ctx->ended = false;
  ctx->failure_reason.clear();
  ctx->last_failure_wake_ns = 0;
  ctx->eos_pushed = false;
  ctx->eos_pending = false;
  prepare_segment_timeline_locked(ctx, load_pts_ns, "initial-load");

  mp_info(ctx->log,
          "Starfish Load: video=%s audio=%s size=%dx%d fps=%d/%d window=%s "
          "dovi=%d pts_to_decode=%" PRId64 " timeline=%.3f\n",
          p.video_codec, p.audio_codec ? p.audio_codec : "(none)", p.width,
          p.height, p.fps_num, p.fps_den,
          p.window_id ? p.window_id : "(acb)", p.dolby_vision,
          p.pts_to_decode_ns, (double)load_pts_ns / 1e9);

  lk.unlock();
  bool ok = ensure_backend(ctx);
  if (ok)
    sf_backend_notify_foreground(ctx->backend);
  if (ok) {
    sf_backend_load_params lp = { payload.c_str() };
    ok = sf_backend_load(ctx->backend, &lp);
  }
  if (ok)
    apply_hdr_info(ctx);
  lk.lock();

  if (!ok) {
    mark_pipeline_failed_locked(ctx, "load-failed");
    wake_all(ctx);
    return false;
  }
  return true;
}

/* ----- packet feeding ---------------------------------------------- */

enum class feed_result { NO_PACKET, SUBMITTED, BLOCKED };

static void queue_ready_frame_locked(starfish_ctx *ctx, int64_t pts_ns,
                                     const char *reason) {
  if (reason && strcmp(reason, "PLAYING packet fallback") == 0) {
    int64_t frame_ns = 50LL * 1000 * 1000;
    if (ctx->fps > 0.0)
      frame_ns = (int64_t)llround(1e9 / ctx->fps);
    pts_ns += frame_ns;
  }
  starfish_video_frame frame = {
      .pts = pts_ns / 1e9,
      .dts = pts_ns / 1e9,
      .duration = ctx->fps > 0 ? 1.0 / ctx->fps : 0.0,
  };
  ctx->ready_frames.push_back(frame);
  ctx->current_pts_ns = pts_ns;
  ctx->min_ready_pts_ns = INT64_MIN;
  ctx->started = true;
  if (reason)
    mp_info(ctx->log, "Starfish queued ready frame after %s pts=%.3f\n",
            reason, (double)pts_ns / 1e9);
}

static void maybe_release_pending_segment_frame_locked(starfish_ctx *ctx,
                                                       int64_t fed_pts_ns) {
  if (!ctx->pending_segment_ready_frame ||
      ctx->pending_segment_ready_await_play)
    return;
  int64_t frame_ns = 50LL * 1000 * 1000;
  if (ctx->fps > 0.0)
    frame_ns = (int64_t)llround(1e9 / ctx->fps);
  if (fed_pts_ns + frame_ns < ctx->pending_segment_ready_pts_ns)
    return;

  const int64_t ready_pts = ctx->pending_segment_ready_pts_ns;
  ctx->pending_segment_ready_frame = false;
  ctx->pending_segment_ready_pts_ns = INT64_MIN;
  ctx->pending_segment_ready_have_pts = false;
  ctx->pending_segment_ready_saw_play = false;
  queue_ready_frame_locked(ctx, ready_pts, "segment target feed");
}

static bool delayed_play_ready_locked(starfish_ctx *ctx) {
  if (!ctx->play_after_preroll_pending || !ctx->play_requested ||
      ctx->fed_video_pts_ns == INT64_MIN)
    return false;

  int64_t frame_ns = 50LL * 1000 * 1000;
  if (ctx->fps > 0.0)
    frame_ns = (int64_t)llround(1e9 / ctx->fps);
  if (ctx->fed_video_pts_ns + frame_ns < ctx->play_after_preroll_target_ns)
    return false;

  return true;
}

static bool maybe_start_delayed_play_locked(starfish_ctx *ctx,
                                            std::unique_lock<std::mutex> &lk) {
  if (!delayed_play_ready_locked(ctx))
    return false;

  const int64_t target_ns = ctx->play_after_preroll_target_ns;
  ctx->play_after_preroll_pending = false;
  ctx->play_after_preroll_target_ns = INT64_MIN;
  if (ctx->pending_segment_ready_frame &&
      ctx->pending_segment_ready_await_play) {
    ctx->pending_segment_ready_watchdog =
        std::chrono::steady_clock::now() + SEGMENT_READY_PLAY_WATCHDOG;
  }
  pipeline_state prev = ctx->state;
  ctx->state = pipeline_state::PLAYING;

  lk.unlock();
  bool ok = sf_backend_play(ctx->backend);
  sf_backend_set_play_rate(ctx->backend, 1000, true);
  lk.lock();

  if (!ok) {
    mp_warn(ctx->log, "Starfish delayed Play failed target=%.3f\n",
            (double)target_ns / 1e9);
    ctx->pending_segment_ready_watchdog =
        std::chrono::steady_clock::time_point::min();
    if (!ctx->stop && !ctx->flush_requested)
      ctx->state = prev;
  } else {
    mp_info(ctx->log, "Starfish Play after preroll target=%.3f fed=%.3f\n",
            (double)target_ns / 1e9, (double)ctx->fed_video_pts_ns / 1e9);
  }
  return true;
}

static bool pending_ready_matches_clock_locked(
    starfish_ctx *ctx, std::unique_lock<std::mutex> &lk, int64_t *ready_ns,
    const char *reason, bool allow_sdk_query);
static int64_t project_fresh_clock_locked(starfish_ctx *ctx, int64_t now);

static bool video_backpressure_ready_locked(starfish_ctx *ctx) {
  if (ctx->video_backpressure_pts_ns == INT64_MIN)
    return false;

  int64_t anchor_ns = project_fresh_clock_locked(ctx, mp_time_ns());
  if (anchor_ns == INT64_MIN)
    anchor_ns = ctx->current_pts_ns;
  if (anchor_ns == INT64_MIN)
    return true;

  return ctx->video_backpressure_pts_ns - anchor_ns <=
         MAX_DECODER_ACCEPT_AHEAD_NS;
}

static void note_bufferlow_progress_locked(starfish_ctx *ctx,
                                           int64_t pts_ns,
                                           int64_t host_ns) {
  if (!ctx->bufferlow_active || pts_ns == INT64_MIN || host_ns <= 0)
    return;
  if (ctx->bufferlow_last_progress_pts_ns == INT64_MIN ||
      pts_ns > ctx->bufferlow_last_progress_pts_ns) {
    ctx->bufferlow_last_progress_pts_ns = pts_ns;
    ctx->bufferlow_last_progress_host_ns = host_ns;
  }
}

static bool bufferlow_stalled_locked(starfish_ctx *ctx, int64_t now) {
  if (!ctx->bufferlow_active || ctx->state != pipeline_state::PLAYING ||
      !ctx->started || ctx->stop || ctx->flush_requested || ctx->need_segment)
    return false;
  if (!ctx->video_queue.empty() || ctx->feed_inflight ||
      ctx->video_backpressure_pts_ns != INT64_MIN)
    return false;

  int64_t last_progress = ctx->bufferlow_last_progress_host_ns > 0
                              ? ctx->bufferlow_last_progress_host_ns
                              : ctx->bufferlow_start_ns;
  if (last_progress <= 0)
    return false;
  return now - last_progress >= BUFFERLOW_STALL_TIMEOUT_NS;
}

static bool maybe_force_pending_segment_ready_locked(
    starfish_ctx *ctx, std::unique_lock<std::mutex> &lk) {
  if (!ctx->pending_segment_ready_frame ||
      !ctx->pending_segment_ready_await_play ||
      ctx->pending_segment_ready_watchdog ==
          std::chrono::steady_clock::time_point::min() ||
      std::chrono::steady_clock::now() < ctx->pending_segment_ready_watchdog)
    return false;

  int64_t ready = ctx->pending_segment_ready_pts_ns;
  // The SDK clock can remain pinned to the pre-seek segment until the new
  // segment produces a frame. The watchdog is the recovery path for that exact
  // no-frame state, so only trust our own fresh sampled clock here.
  if (!pending_ready_matches_clock_locked(ctx, lk, &ready, "watchdog", false) ||
      !ctx->pending_segment_ready_frame ||
      ctx->pending_segment_ready_pts_ns != ready)
    return false;

  mp_warn(ctx->log,
          "Starfish PLAYING watchdog tripped, force-queue ready pts=%.3f\n",
          (double)ready / 1e9);
  ctx->pending_segment_ready_frame = false;
  ctx->pending_segment_ready_pts_ns = INT64_MIN;
  ctx->pending_segment_ready_have_pts = false;
  ctx->pending_segment_ready_saw_play = false;
  ctx->pending_segment_ready_await_play = false;
  ctx->pending_segment_ready_watchdog =
      std::chrono::steady_clock::time_point::min();
  queue_ready_frame_locked(ctx, ready, "watchdog");
  lk.unlock();
  wake_stream(ctx, STARFISH_STREAM_VIDEO);
  lk.lock();
  return true;
}

static feed_result try_drain(starfish_ctx *ctx,
                             std::unique_lock<std::mutex> &lk,
                             enum starfish_stream_type stream) {
  if (ctx->need_segment && stream == STARFISH_STREAM_AUDIO &&
      !ctx->video_codec.empty())
    return feed_result::NO_PACKET;

  std::deque<queued_packet> *queue = stream == STARFISH_STREAM_VIDEO
                                         ? &ctx->video_queue
                                         : &ctx->audio_queue;
  size_t *queue_bytes = stream == STARFISH_STREAM_VIDEO
                            ? &ctx->video_queue_bytes
                            : &ctx->audio_queue_bytes;
  if (queue->empty())
    return feed_result::NO_PACKET;

  queued_packet packet = queue->front();
  if (ctx->current_pts_ns != INT64_MIN &&
      packet.pts_ns - ctx->current_pts_ns > MAX_FEED_AHEAD_NS)
    return feed_result::BLOCKED;

  const bool audio_only = ctx->video_codec.empty();
  const bool do_segment =
      ctx->need_segment && (stream == STARFISH_STREAM_VIDEO || audio_only);
  int64_t segment_pts_ns = INT64_MIN;
  bool prime_audio = false;

  if (do_segment) {
    const int64_t requested_pts_ns = ctx->pending_seek_target
                                         ? ctx->pending_seek_target_ns
                                         : packet.pts_ns;
    segment_pts_ns = choose_segment_start_ns(ctx, packet.pts_ns);
    ctx->need_segment = false;
    ctx->pending_seek_target = false;
    ctx->seek_target_ns = segment_pts_ns;
    prepare_segment_timeline_locked(ctx, segment_pts_ns, "segment-packet");
    if (requested_pts_ns != segment_pts_ns) {
      mp_info(ctx->log,
              "Starfish segment start adjusted packet=%.3f requested=%.3f start=%.3f\n",
              (double)packet.pts_ns / 1e9, (double)requested_pts_ns / 1e9,
              (double)segment_pts_ns / 1e9);
    }
    if (ctx->play_requested) {
      ctx->play_after_preroll_pending = true;
      ctx->play_after_preroll_target_ns = segment_pts_ns;
    }
    if (ctx->need_audio) {
      prime_audio = true;
    }
    if (stream == STARFISH_STREAM_VIDEO) {
      ctx->pending_segment_ready_frame = true;
      ctx->pending_segment_ready_pts_ns = segment_pts_ns;
      ctx->pending_segment_ready_have_pts = false;
      ctx->pending_segment_ready_saw_play = false;
      ctx->pending_segment_ready_await_play = true;
      ctx->pending_segment_ready_watchdog =
          std::chrono::steady_clock::time_point::min();
      ctx->first_segment_after_load = false;
    }
  }

  lk.unlock();
  if (prime_audio) {
    bool primed = call_audio_prime(&ctx->audio_prime, segment_pts_ns);
    mp_info(ctx->log,
            "Starfish requested audio segment prime pts=%.3f result=%d\n",
            (double)segment_pts_ns / 1e9, primed);
  }

  if (do_segment) {
    const int64_t decode_pts_ns =
        decode_start_for_segment_ns(ctx, segment_pts_ns);
    if (!sf_backend_set_time_to_decode(ctx->backend, decode_pts_ns))
      mp_warn(ctx->log, "Starfish setTimeToDecode failed target=%" PRId64 "\n",
              decode_pts_ns);
    if (!sf_backend_send_segment_event(ctx->backend))
      mp_warn(ctx->log, "Starfish sendSegmentEvent failed\n");
    mp_info(ctx->log, "Starfish segment restart sent decode=%.3f timeline=%.3f\n",
            (double)decode_pts_ns / 1e9, (double)segment_pts_ns / 1e9);
  }

  // separatedPTS accepts presentation timestamps in decode order. Keep the
  // SDK feed, FRAMEREADY events, and getCurrentPlaytime on that one timeline.
  sf_backend_packet bp = {
      .stream = stream == STARFISH_STREAM_VIDEO ? SF_BACKEND_VIDEO
                                                : SF_BACKEND_AUDIO,
      .data = packet.data->data(),
      .size = packet.data->size(),
      .pts_ns = packet.pts_ns,
  };
  lk.lock();
  ctx->feed_inflight = true;
  ctx->feed_inflight_stream = stream;
  ctx->feed_inflight_start_ns = mp_time_ns();
  ctx->feed_inflight_media_pts_ns = packet.pts_ns;
  ctx->feed_inflight_sdk_pts_ns = bp.pts_ns;
  ctx->feed_inflight_size = packet.data->size();
  lk.unlock();

  sf_backend_feed_result r = sf_backend_feed(ctx->backend, &bp);
  const int64_t feed_done_host_ns = mp_time_ns();
  lk.lock();

  ctx->feed_inflight = false;
  ctx->last_feed_result = r;
  ctx->last_feed_stream = stream;
  ctx->last_feed_done_ns = feed_done_host_ns;
  ctx->last_feed_media_pts_ns = packet.pts_ns;
  ctx->last_feed_sdk_pts_ns = bp.pts_ns;

  if (ctx->stop || ctx->flush_requested)
    return feed_result::BLOCKED;

  if (r == SF_BACKEND_FEED_BUFFER_FULL || r == SF_BACKEND_FEED_RETRY) {
    if (r == SF_BACKEND_FEED_BUFFER_FULL) {
      int *logs = stream == STARFISH_STREAM_VIDEO ? &ctx->video_bufferfull_logs
                                                  : &ctx->audio_bufferfull_logs;
      if (*logs < 8) {
        mp_info(ctx->log,
                "Starfish %s BufferFull pts=%.3f feed=%.3f queue=%.2fMB\n",
                stream == STARFISH_STREAM_VIDEO ? "video" : "audio",
                (double)packet.pts_ns / 1e9,
                (double)bp.pts_ns / 1e9, *queue_bytes / 1024.0 / 1024.0);
        (*logs)++;
      }
    }
    return feed_result::BLOCKED;
  }
  if (r == SF_BACKEND_FEED_ERROR) {
    mp_warn(ctx->log, "Starfish %s feed Error pts=%" PRId64 " (dropping)\n",
            stream == STARFISH_STREAM_VIDEO ? "video" : "audio",
            packet.pts_ns);
    // Drop the packet so we don't spin retrying the same bad data.
  }
  const bool feed_ok = r == SF_BACKEND_FEED_OK;

  *queue_bytes -= packet.data->size();
  queue->pop_front();

  if (do_segment && stream == STARFISH_STREAM_VIDEO) {
    mp_info(ctx->log,
            "Starfish segment decode started packet_pts=%.3f target=%.3f\n",
            (double)packet.pts_ns / 1e9, (double)segment_pts_ns / 1e9);
  }
  if (stream == STARFISH_STREAM_VIDEO && feed_ok) {
    // B-frame packet PTS can move backwards in decode order.
    ctx->fed_video_pts_ns = std::max(ctx->fed_video_pts_ns, packet.pts_ns);
    maybe_start_delayed_play_locked(ctx, lk);
    maybe_release_pending_segment_frame_locked(ctx, packet.pts_ns);
  } else if (stream == STARFISH_STREAM_AUDIO && feed_ok) {
    ctx->fed_audio_pts_ns = packet.pts_ns;
  }

  lk.unlock();
  wake_stream(ctx, stream);
  lk.lock();
  return feed_result::SUBMITTED;
}

/* ----- clock sampler ----------------------------------------------- */

static bool clock_ahead_of_fed_video_locked(starfish_ctx *ctx,
                                            int64_t clock_ns) {
  return ctx->fed_video_pts_ns != INT64_MIN &&
         clock_ns > ctx->fed_video_pts_ns + CLOCK_FED_VIDEO_SLACK_NS;
}

static void log_clock_reject_locked(starfish_ctx *ctx, const char *reason,
                                    int64_t pts_ns) {
  const int64_t now = mp_time_ns();
  ctx->clock_reject_suppressed++;
  if (ctx->last_clock_reject_log_ns &&
      now - ctx->last_clock_reject_log_ns < WORKER_STATUS_PERIOD_NS)
    return;

  mp_warn(ctx->log,
          "Starfish rejected %s clock pts=%.3f current=%.3f fed_v=%.3f "
          "fed_a=%.3f suppressed=%d\n",
          reason ? reason : "implausible", (double)pts_ns / 1e9,
          ns_to_sec_or_neg(ctx->current_pts_ns),
          ns_to_sec_or_neg(ctx->fed_video_pts_ns),
          ns_to_sec_or_neg(ctx->fed_audio_pts_ns),
          ctx->clock_reject_suppressed - 1);
  ctx->last_clock_reject_log_ns = now;
  ctx->clock_reject_suppressed = 0;
}

static bool clock_sample_plausible_locked(starfish_ctx *ctx, int64_t pts_ns) {
  if (clock_ahead_of_fed_video_locked(ctx, pts_ns)) {
    log_clock_reject_locked(ctx, "ahead-of-fed-video", pts_ns);
    return false;
  }

  if (!ctx->clock_export_ready && ctx->current_pts_ns != INT64_MIN &&
      llabs(pts_ns - ctx->current_pts_ns) >
          PENDING_PLAYING_CLOCK_TOLERANCE_NS) {
    log_clock_reject_locked(ctx, "off-segment", pts_ns);
    return false;
  }

  return true;
}

// quantized_sample: pts_ns came straight from getCurrentPlaytime, which is
// frame-quantized (it reports the pts of the frame currently on screen and
// holds it for the whole frame duration). Non-quantized callers pass a
// projected/derived pts where the flip-bracketing below does not apply.
static int64_t accept_clock_sample_locked(starfish_ctx *ctx, int64_t pts_ns,
                                          int64_t host_time_ns,
                                          bool quantized_sample) {
  const int64_t prev_poll_ns = ctx->clock_last_poll_host_ns;
  int64_t anchor_ns = host_time_ns;

  if (ctx->clock_sample_valid) {
    const int64_t old_ns = (int64_t)llround(ctx->clock_sample_pts * 1e9);
    if (pts_ns < old_ns)
      pts_ns = old_ns;
    if (quantized_sample && pts_ns == old_ns) {
      // Same frame still on screen. Keep the anchor from the poll that first
      // reported this pts (~ the flip time). Moving it forward on every poll
      // made pts+age projections reset toward the frame-start pts each poll,
      // under-reporting the video position by up to a frame duration (mean
      // ~half a frame) -- audio slaved to the clock then ran early by the
      // same amount.
      ctx->clock_last_poll_host_ns = host_time_ns;
      if (pts_ns > ctx->current_pts_ns)
        ctx->current_pts_ns = pts_ns;
      return pts_ns;
    }
    if (quantized_sample && prev_poll_ns > 0 && host_time_ns > prev_poll_ns &&
        host_time_ns - prev_poll_ns <= 2 * CLOCK_SAMPLE_PERIOD_NS) {
      // pts advanced: the flip happened between the previous poll and this
      // one. The midpoint halves the worst-case anchor error vs. taking the
      // poll time itself. After long poll gaps (stall, slow SDK call) the
      // bracket is too wide to be useful; keep the poll time then.
      anchor_ns = prev_poll_ns + (host_time_ns - prev_poll_ns) / 2;
    }
  }

  ctx->clock_sample_valid = true;
  ctx->clock_sample_pts = (double)pts_ns / 1e9;
  ctx->clock_sample_host_ns = anchor_ns;
  if (quantized_sample)
    ctx->clock_last_poll_host_ns = host_time_ns;
  if (pts_ns > ctx->current_pts_ns)
    ctx->current_pts_ns = pts_ns;
  return pts_ns;
}

static void sample_clock_if_due(starfish_ctx *ctx,
                                std::unique_lock<std::mutex> &lk) {
  if (ctx->state != pipeline_state::PLAYING || !ctx->started)
    return;
  const int64_t now = mp_time_ns();
  if (ctx->last_clock_attempt_ns &&
      now - ctx->last_clock_attempt_ns < CLOCK_SAMPLE_PERIOD_NS)
    return;
  ctx->last_clock_attempt_ns = now;

  lk.unlock();
  sf_backend_clock_sample sample = {};
  bool ok = sf_backend_get_current_playtime(ctx->backend, &sample);
  lk.lock();

  if (!ok || !sample.valid)
    return;
  if (sample.query_duration_ns > CLOCK_SAMPLE_SLOW_NS) {
    mp_trace(ctx->log, "Starfish clock sample rejected (slow %.1fms)\n",
             sample.query_duration_ns / 1e6);
    return;
  }
  int64_t new_ns = (int64_t)llround(sample.pts * 1e9);
  if (!clock_sample_plausible_locked(ctx, new_ns))
    return;

  bool had_clock = ctx->clock_sample_valid;
  int64_t old_ns = INT64_MIN;
  if (had_clock) {
    old_ns = (int64_t)llround(ctx->clock_sample_pts * 1e9);
    if (new_ns + CLOCK_BACKWARD_TOLERANCE_NS < old_ns) {
      if (clock_ahead_of_fed_video_locked(ctx, old_ns)) {
        mp_warn(ctx->log,
                "Starfish replacing stale accepted clock old=%.3f new=%.3f "
                "fed_v=%.3f\n",
                (double)old_ns / 1e9, (double)new_ns / 1e9,
                ns_to_sec_or_neg(ctx->fed_video_pts_ns));
        ctx->clock_sample_valid = false;
        ctx->clock_export_ready = false;
        ctx->clock_stability_probe_pts_ns = INT64_MIN;
        ctx->clock_stability_probe_host_ns = 0;
        had_clock = false;
      } else {
        const int64_t jump_now = mp_time_ns();
        ctx->clock_jump_suppressed++;
        if (!ctx->last_clock_jump_log_ns ||
            jump_now - ctx->last_clock_jump_log_ns >= WORKER_STATUS_PERIOD_NS) {
          mp_verbose(ctx->log,
                     "Starfish clock jumped back old=%.3f new=%.3f "
                     "suppressed=%d\n",
                     ctx->clock_sample_pts, sample.pts,
                     ctx->clock_jump_suppressed - 1);
          ctx->last_clock_jump_log_ns = jump_now;
          ctx->clock_jump_suppressed = 0;
        }
        return;
      }
    }
    if (new_ns < old_ns)
      new_ns = old_ns;
  }

  if (had_clock) {
    if (!ctx->clock_export_ready) {
      if (ctx->clock_stability_probe_pts_ns == INT64_MIN ||
          ctx->clock_stability_probe_host_ns <= 0 ||
          sample.host_time_ns < ctx->clock_stability_probe_host_ns) {
        ctx->clock_stability_probe_pts_ns = old_ns;
        ctx->clock_stability_probe_host_ns = ctx->clock_sample_host_ns;
      }

      const int64_t wall_delta_ns =
          sample.host_time_ns - ctx->clock_stability_probe_host_ns;
      const int64_t pts_delta_ns =
          new_ns - ctx->clock_stability_probe_pts_ns;
      if (wall_delta_ns >= CLOCK_EXPORT_STABLE_WINDOW_NS) {
        const double rate = pts_delta_ns > 0
                                ? (double)pts_delta_ns / wall_delta_ns
                                : 0.0;
        if (pts_delta_ns >= 0 && rate >= CLOCK_EXPORT_MIN_RATE &&
            rate <= CLOCK_EXPORT_MAX_RATE) {
          ctx->clock_export_ready = true;
          mp_info(ctx->log,
                  "Starfish clock export ready pts=%.3f window=%.3f rate=%.3f\n",
                  sample.pts, wall_delta_ns / 1e9, rate);
        } else {
          ctx->clock_stability_probe_pts_ns = new_ns;
          ctx->clock_stability_probe_host_ns = sample.host_time_ns;
        }
      }
    }
  } else {
    ctx->clock_stability_probe_pts_ns = new_ns;
    ctx->clock_stability_probe_host_ns = sample.host_time_ns;
  }
  new_ns = accept_clock_sample_locked(ctx, new_ns, sample.host_time_ns, true);
  if (ctx->log_next_clock_sample) {
    mp_info(ctx->log, "Starfish clock sample pts=%.3f query=%.1fms\n",
            (double)new_ns / 1e9, sample.query_duration_ns / 1e6);
    ctx->log_next_clock_sample = false;
  }
}

static bool sample_clock_now_locked(starfish_ctx *ctx,
                                    std::unique_lock<std::mutex> &lk,
                                    sf_backend_clock_sample *sample) {
  if (!ctx->backend || !sample)
    return false;

  lk.unlock();
  bool ok = sf_backend_get_current_playtime(ctx->backend, sample);
  lk.lock();

  return ok && sample->valid &&
         sample->query_duration_ns <= CLOCK_SAMPLE_SLOW_NS;
}

static int64_t project_fresh_clock_locked(starfish_ctx *ctx, int64_t now) {
  if (!ctx->clock_sample_valid || ctx->clock_sample_host_ns <= 0)
    return INT64_MIN;

  const int64_t age = now - ctx->clock_sample_host_ns;
  if (age < 0 || age > CLOCK_FRESHNESS_NS)
    return INT64_MIN;

  const double pts = ctx->clock_sample_pts + age / 1e9;
  if (pts == MP_NOPTS_VALUE || !std::isfinite(pts))
    return INT64_MIN;
  return (int64_t)llround(pts * 1e9);
}

static bool pending_ready_matches_clock_locked(
    starfish_ctx *ctx, std::unique_lock<std::mutex> &lk, int64_t *ready_ns,
    const char *reason, bool allow_sdk_query) {
  if (!ready_ns)
    return false;

  int64_t clock_ns = INT64_MIN;
  int64_t clock_host_ns = 0;
  if (allow_sdk_query) {
    sf_backend_clock_sample sample = {};
    if (!sample_clock_now_locked(ctx, lk, &sample))
      return true;
    clock_ns = (int64_t)llround(sample.pts * 1e9);
    clock_host_ns = sample.host_time_ns;
  } else {
    const int64_t now = mp_time_ns();
    clock_ns = project_fresh_clock_locked(ctx, now);
    if (clock_ns == INT64_MIN)
      return true;
    clock_host_ns = now;
  }

  const int64_t delta_ns = llabs(clock_ns - *ready_ns);
  if (delta_ns > PENDING_PLAYING_CLOCK_TOLERANCE_NS) {
    mp_info(ctx->log,
            "Starfish ignoring stale %s for pending segment ready=%.3f clock=%.3f delta=%.3f\n",
            reason ? reason : "event", (double)*ready_ns / 1e9,
            (double)clock_ns / 1e9,
            (double)delta_ns / 1e9);
    ctx->pending_segment_ready_watchdog =
        std::chrono::steady_clock::now() + BUFFERFULL_BACKOFF;
    return false;
  }

  clock_ns = accept_clock_sample_locked(ctx, clock_ns, clock_host_ns,
                                        allow_sdk_query);
  if (clock_ns != *ready_ns) {
    mp_info(ctx->log,
            "Starfish using clock for pending %s ready packet=%.3f clock=%.3f\n",
            reason ? reason : "event", (double)*ready_ns / 1e9,
            (double)clock_ns / 1e9);
    *ready_ns = clock_ns;
    ctx->pending_segment_ready_pts_ns = clock_ns;
  }
  return true;
}

/* ----- worker thread ----------------------------------------------- */

static void apply_flush_locked(starfish_ctx *ctx,
                               std::unique_lock<std::mutex> &lk) {
  bool loaded = is_loaded_state(ctx->state);
  ctx->ended = false;
  ctx->eos_pushed = false;
  ctx->eos_pending = false;
  ctx->need_segment = true;
  ctx->flush_requested = false;
  clear_queues_locked(ctx);
  if (ctx->pending_seek_target)
    prepare_segment_timeline_locked(ctx, ctx->pending_seek_target_ns,
                                    "seek-flush");

  if (loaded) {
    lk.unlock();
    mp_info(ctx->log, "Starfish flush applied\n");
    sf_backend_flush(ctx->backend);
    lk.lock();
  }

  ctx->cv.notify_all();
  lk.unlock();
  wake_all(ctx);
  lk.lock();
}

static bool should_wake_synthetic_video_locked(starfish_ctx *ctx) {
  return ctx->state == pipeline_state::PLAYING && ctx->started &&
         !ctx->need_segment && !ctx->pending_segment_ready_frame;
}

static void worker_loop(starfish_ctx *ctx) {
  std::unique_lock<std::mutex> lk(ctx->lock);

  while (!ctx->stop) {
    if (ctx->flush_requested) {
      apply_flush_locked(ctx, lk);
      continue;
    }

    if (can_load(ctx)) {
      if (!try_start_load(ctx, lk))
        continue;
      continue;
    }

    if (ctx->state == pipeline_state::FAILED) {
      const int64_t now = mp_time_ns();
      if (!ctx->last_failure_wake_ns ||
          now - ctx->last_failure_wake_ns >= FAILURE_WAKE_PERIOD_NS) {
        if (!ctx->last_failure_wake_ns)
          log_worker_status_locked(ctx, ctx->failure_reason.empty()
                                            ? "failed"
                                            : ctx->failure_reason.c_str());
        ctx->last_failure_wake_ns = now;
        lk.unlock();
        wake_all(ctx);
        lk.lock();
        continue;
      }
      ctx->cv.wait_for(lk, std::chrono::milliseconds(FAILURE_WAKE_PERIOD_NS /
                                                     (1000 * 1000)), [&] {
        return ctx->stop || ctx->flush_requested ||
               ctx->state != pipeline_state::FAILED;
      });
      continue;
    }

    if (maybe_force_pending_segment_ready_locked(ctx, lk))
      continue;

    if (is_loaded_state(ctx->state)) {
      /* Play/Pause transitions */
      if (maybe_start_delayed_play_locked(ctx, lk))
        continue;
      if (!ctx->play_requested && ctx->state == pipeline_state::PLAYING) {
        ctx->state = pipeline_state::PAUSED;
        lk.unlock();
        bool ok = sf_backend_pause(ctx->backend);
        lk.lock();
        if (!ok) {
          mp_warn(ctx->log, "Starfish Pause failed\n");
          ctx->state = pipeline_state::PLAYING;
        } else {
          mp_info(ctx->log, "Starfish Pause (user)\n");
        }
        continue;
      }
      if (ctx->play_requested &&
          !ctx->need_segment && !ctx->play_after_preroll_pending &&
          (ctx->state == pipeline_state::PAUSED ||
           ctx->state == pipeline_state::LOADED)) {
        pipeline_state prev = ctx->state;
        ctx->state = pipeline_state::PLAYING;
        lk.unlock();
        bool ok = sf_backend_play(ctx->backend);
        sf_backend_set_play_rate(ctx->backend, 1000, true);
        lk.lock();
        if (!ok) {
          mp_warn(ctx->log, "Starfish Play failed\n");
          ctx->state = prev;
        } else {
          mp_info(ctx->log, "Starfish Play (user)\n");
        }
        continue;
      }

      /* Feed packets */
      if (should_feed_packets_locked(ctx) &&
          (!ctx->video_queue.empty() || !ctx->audio_queue.empty())) {
        const bool have_video = !ctx->video_queue.empty();
        const bool have_audio = !ctx->audio_queue.empty();
        const bool segment_audio_preroll =
            ctx->need_audio && have_audio &&
            ctx->pending_segment_ready_frame &&
            ctx->pending_segment_ready_pts_ns != INT64_MIN &&
            ctx->audio_queue.front().pts_ns <=
                ctx->pending_segment_ready_pts_ns + MAX_FEED_AHEAD_NS;
        const bool prefer_audio =
            !ctx->need_segment && have_audio &&
            (segment_audio_preroll ||
             !have_video || ctx->audio_queue.front().pts_ns <=
                              ctx->video_queue.front().pts_ns);
        enum starfish_stream_type first =
            prefer_audio ? STARFISH_STREAM_AUDIO : STARFISH_STREAM_VIDEO;
        enum starfish_stream_type second =
            prefer_audio ? STARFISH_STREAM_VIDEO : STARFISH_STREAM_AUDIO;

        feed_result r1 = try_drain(ctx, lk, first);
        if (r1 == feed_result::SUBMITTED) {
          sample_clock_if_due(ctx, lk);
          continue;
        }
        feed_result r2 = try_drain(ctx, lk, second);
        if (r2 == feed_result::SUBMITTED) {
          sample_clock_if_due(ctx, lk);
          continue;
        }

        if (r1 == feed_result::BLOCKED || r2 == feed_result::BLOCKED) {
          sample_clock_if_due(ctx, lk);
          maybe_log_worker_status_locked(ctx, "feed-blocked");
          const bool wake_video = should_wake_synthetic_video_locked(ctx);
          lk.unlock();
          if (wake_video)
            wake_stream(ctx, STARFISH_STREAM_VIDEO);
          std::this_thread::sleep_for(BUFFERFULL_BACKOFF);
          lk.lock();
          continue;
        }
      }

      /* EOS */
      if (ctx->eos_pending && !ctx->eos_pushed && ctx->video_queue.empty() &&
          ctx->audio_queue.empty()) {
        ctx->eos_pushed = true;
        lk.unlock();
        if (!sf_backend_push_eos(ctx->backend))
          mp_warn(ctx->log, "Starfish pushEOS failed\n");
        lk.lock();
        ctx->eos_pending = false;
        continue;
      }

      sample_clock_if_due(ctx, lk);
      note_bufferlow_progress_locked(ctx, ctx->current_pts_ns, mp_time_ns());
      if (video_backpressure_ready_locked(ctx)) {
        mp_info(ctx->log,
                "Starfish decoder backpressure released packet=%.3f "
                "current=%.3f\n",
                (double)ctx->video_backpressure_pts_ns / 1e9,
                ns_to_sec_or_neg(ctx->current_pts_ns));
        ctx->video_backpressure_pts_ns = INT64_MIN;
        lk.unlock();
        wake_stream(ctx, STARFISH_STREAM_VIDEO);
        lk.lock();
        continue;
      }
      if (bufferlow_stalled_locked(ctx, mp_time_ns())) {
        log_worker_status_locked(ctx, "bufferlow-stall");
        mp_err(ctx->log,
               "Starfish BUFFERLOW stalled with no video progress; failing pipeline\n");
        mark_pipeline_failed_locked(ctx, "bufferlow-stall");
        clear_queues_locked(ctx);
        lk.unlock();
        wake_stream(ctx, STARFISH_STREAM_VIDEO);
        wake_stream(ctx, STARFISH_STREAM_AUDIO);
        lk.lock();
        continue;
      }
      if (ctx->state == pipeline_state::PLAYING && ctx->started &&
          ctx->clock_sample_valid && ctx->clock_sample_host_ns > 0 &&
          mp_time_ns() - ctx->clock_sample_host_ns >
              WORKER_STATUS_PERIOD_NS)
        maybe_log_worker_status_locked(ctx, "clock-stale");
      if (should_wake_synthetic_video_locked(ctx)) {
        lk.unlock();
        wake_stream(ctx, STARFISH_STREAM_VIDEO);
        lk.lock();
      }
    }

    auto wait = std::chrono::steady_clock::duration(WORKER_IDLE_WAIT);
    ctx->cv.wait_for(lk, wait, [&] {
      return ctx->stop || ctx->flush_requested || can_load(ctx) ||
             (is_loaded_state(ctx->state) &&
              ((should_feed_packets_locked(ctx) &&
                (!ctx->video_queue.empty() || !ctx->audio_queue.empty())) ||
               (ctx->eos_pending && !ctx->eos_pushed) ||
               (ctx->play_requested &&
                !ctx->need_segment && !ctx->play_after_preroll_pending &&
                (ctx->state == pipeline_state::PAUSED ||
                 ctx->state == pipeline_state::LOADED)) ||
               delayed_play_ready_locked(ctx) ||
               (!ctx->play_requested &&
                ctx->state == pipeline_state::PLAYING)));
    });
  }
}

/* ----- backend event callback (Starfish callback thread) ----------- */

static void backend_event_cb(void *opaque, enum sf_backend_event_type type,
                             int64_t num_value, const char *str_value) {
  auto *ctx = static_cast<starfish_ctx *>(opaque);
  std::unique_lock<std::mutex> lk(ctx->lock);
  if (ctx->stop)
    return;

  bool wake_video = false;
  bool wake_audio = false;
  const int64_t event_host_ns = mp_time_ns();

  switch (type) {
  case SF_EVENT_FRAME_READY: {
    int64_t pts_ns = num_value;
    if (ctx->flush_requested || ctx->need_segment ||
        ctx->state == pipeline_state::IDLE ||
        ctx->state == pipeline_state::FAILED)
      break;
    if (ctx->min_ready_pts_ns != INT64_MIN &&
        pts_ns + STALE_READY_TOLERANCE_NS < ctx->min_ready_pts_ns) {
      mp_info(ctx->log,
              "Starfish dropping stale ready frame pts=%.3f min=%.3f\n",
              (double)pts_ns / 1e9, (double)ctx->min_ready_pts_ns / 1e9);
      break;
    }
    if (ctx->fed_video_pts_ns != INT64_MIN &&
        pts_ns > ctx->fed_video_pts_ns + READY_CEILING_SLACK_NS) {
      mp_info(ctx->log, "Starfish dropping stale frame pts=%.3f\n",
              (double)pts_ns / 1e9);
      break;
    }
    note_bufferlow_progress_locked(ctx, pts_ns, event_host_ns);
    if (ctx->pending_segment_ready_frame &&
        ctx->pending_segment_ready_await_play) {
      ctx->pending_segment_ready_pts_ns = pts_ns;
      ctx->pending_segment_ready_have_pts = true;
      if (!ctx->pending_segment_ready_saw_play) {
        mp_info(ctx->log,
                "Starfish deferred ready frame until PLAYING pts=%.3f\n",
                (double)pts_ns / 1e9);
        break;
      }

      ctx->pending_segment_ready_frame = false;
      ctx->pending_segment_ready_pts_ns = INT64_MIN;
      ctx->pending_segment_ready_have_pts = false;
      ctx->pending_segment_ready_saw_play = false;
      ctx->pending_segment_ready_await_play = false;
      ctx->pending_segment_ready_watchdog =
          std::chrono::steady_clock::time_point::min();
      queue_ready_frame_locked(ctx, pts_ns, "deferred FRAMEREADY");
      wake_video = true;
      break;
    }
    queue_ready_frame_locked(ctx, pts_ns, nullptr);
    wake_video = true;
    break;
  }
  case SF_EVENT_LOAD_COMPLETED: {
    mp_info(ctx->log, "Starfish StateUpdate: LOADCOMPLETED\n");
    ctx->state = pipeline_state::LOADED;
    ctx->need_segment = true;
    ctx->ended = false;
    ctx->first_segment_after_load = true;
    const char *media_id = sf_backend_get_media_id(ctx->backend);
    sf_backend_acb *acb = ctx->acb;
    if (acb) {
      lk.unlock();
      sf_backend_acb_attach(acb, media_id);
      lk.lock();
    }
    wake_video = true;
    wake_audio = true;
    break;
  }
  case SF_EVENT_PRELOAD_COMPLETED:
    mp_info(ctx->log, "Starfish StateUpdate: PRELOADCOMPLETED\n");
    break;
  case SF_EVENT_PLAYING:
    mp_info(ctx->log, "Starfish StateUpdate: PLAYING\n");
    ctx->state = pipeline_state::PLAYING;
    if (ctx->pending_segment_ready_frame &&
        ctx->pending_segment_ready_await_play)
      ctx->pending_segment_ready_saw_play = true;
    if (ctx->pending_segment_ready_frame &&
        ctx->pending_segment_ready_await_play &&
        ctx->pending_segment_ready_have_pts) {
      int64_t ready = ctx->pending_segment_ready_pts_ns;
      if (!pending_ready_matches_clock_locked(ctx, lk, &ready, "PLAYING",
                                              false) ||
          !ctx->pending_segment_ready_frame ||
          ctx->pending_segment_ready_pts_ns != ready)
        break;

      ctx->pending_segment_ready_frame = false;
      ctx->pending_segment_ready_pts_ns = INT64_MIN;
      ctx->pending_segment_ready_have_pts = false;
      ctx->pending_segment_ready_saw_play = false;
      ctx->pending_segment_ready_await_play = false;
      ctx->pending_segment_ready_watchdog =
          std::chrono::steady_clock::time_point::min();
      queue_ready_frame_locked(ctx, ready, "PLAYING state update");
    } else if (ctx->pending_segment_ready_frame &&
               ctx->pending_segment_ready_await_play) {
      int64_t ready = ctx->pending_segment_ready_pts_ns;
      if (!pending_ready_matches_clock_locked(ctx, lk, &ready, "PLAYING",
                                              false) ||
          !ctx->pending_segment_ready_frame ||
          ctx->pending_segment_ready_pts_ns != ready)
        break;

      mp_info(ctx->log,
              "Starfish PLAYING without FRAMEREADY; using pts=%.3f\n",
              (double)ready / 1e9);
      ctx->pending_segment_ready_frame = false;
      ctx->pending_segment_ready_pts_ns = INT64_MIN;
      ctx->pending_segment_ready_have_pts = false;
      ctx->pending_segment_ready_saw_play = false;
      ctx->pending_segment_ready_await_play = false;
      ctx->pending_segment_ready_watchdog =
          std::chrono::steady_clock::time_point::min();
      queue_ready_frame_locked(ctx, ready, "PLAYING packet fallback");
    }
    if (ctx->acb) {
      sf_backend_acb *acb = ctx->acb;
      lk.unlock();
      sf_backend_acb_set_play_state(acb, SF_ACB_PLAYING);
      lk.lock();
    }
    wake_video = true;
    wake_audio = true;
    break;
  case SF_EVENT_PAUSED:
    mp_info(ctx->log, "Starfish StateUpdate: PAUSED\n");
    ctx->state = pipeline_state::PAUSED;
    if (ctx->acb) {
      sf_backend_acb *acb = ctx->acb;
      lk.unlock();
      sf_backend_acb_set_play_state(acb, SF_ACB_PAUSED);
      lk.lock();
    }
    break;
  case SF_EVENT_SEEK_DONE:
    mp_info(ctx->log, "Starfish StateUpdate: SEEKDONE\n");
    ctx->clock_sample_valid = false;
    ctx->clock_export_ready = false;
    ctx->clock_stability_probe_pts_ns = INT64_MIN;
    ctx->clock_stability_probe_host_ns = 0;
    wake_video = true;
    wake_audio = true;
    break;
  case SF_EVENT_END_OF_STREAM:
    mp_info(ctx->log, "Starfish StateUpdate: ENDOFSTREAM\n");
    ctx->ended = true;
    wake_video = true;
    wake_audio = true;
    break;
  case SF_EVENT_UNLOAD_COMPLETED:
    mp_info(ctx->log, "Starfish StateUpdate: UNLOADCOMPLETED\n");
    ctx->state = pipeline_state::IDLE;
    ctx->ended = true;
    clear_queues_locked(ctx);
    if (ctx->acb) {
      sf_backend_acb *acb = ctx->acb;
      lk.unlock();
      sf_backend_acb_set_play_state(acb, SF_ACB_UNLOADED);
      lk.lock();
    }
    wake_video = true;
    wake_audio = true;
    break;
  case SF_EVENT_VIDEO_INFO:
    mp_info(ctx->log, "Starfish Event: VIDEO_INFO %s\n",
            str_value ? str_value : "");
    if (ctx->acb && str_value) {
      sf_backend_acb *acb = ctx->acb;
      std::string payload = str_value;
      lk.unlock();
      sf_backend_acb_set_video_info(acb, payload.c_str());
      lk.lock();
    }
    break;
  case SF_EVENT_AUDIO_INFO:
    mp_info(ctx->log, "Starfish Event: AUDIO_INFO %s\n",
            str_value ? str_value : "");
    break;
  case SF_EVENT_BUFFER_LOW:
    mp_info(ctx->log, "Starfish Event: BUFFERLOW\n");
    ctx->bufferlow_active = true;
    ctx->bufferlow_start_ns = event_host_ns;
    ctx->bufferlow_last_progress_pts_ns = ctx->current_pts_ns;
    ctx->bufferlow_last_progress_host_ns = event_host_ns;
    log_worker_status_locked(ctx, "bufferlow");
    wake_video = true;
    wake_audio = true;
    break;
  case SF_EVENT_BUFFER_FULL:
    if (ctx->bufferlow_active) {
      mp_info(ctx->log,
              "Starfish Event: BUFFERFULL recovered after %.1fms current=%.3f\n",
              (event_host_ns - ctx->bufferlow_start_ns) / 1e6,
              ns_to_sec_or_neg(ctx->current_pts_ns));
    } else {
      mp_trace(ctx->log, "Starfish Event: BUFFERFULL\n");
    }
    ctx->bufferlow_active = false;
    wake_video = true;
    wake_audio = true;
    break;
  case SF_EVENT_ERROR:
    mp_err(ctx->log, "Starfish error num=%" PRId64 " str=%s\n", num_value,
           str_value ? str_value : "");
    log_worker_status_locked(ctx, "error");
    mark_pipeline_failed_locked(ctx, "backend-error");
    clear_queues_locked(ctx);
    wake_video = true;
    wake_audio = true;
    break;
  case SF_EVENT_UNKNOWN:
  default:
    break;
  }

  ctx->cv.notify_all();
  lk.unlock();
  if (wake_video)
    wake_stream(ctx, STARFISH_STREAM_VIDEO);
  if (wake_audio)
    wake_stream(ctx, STARFISH_STREAM_AUDIO);
}

/* ===================================================================
 * Public C API
 * =================================================================== */

extern "C" {

struct starfish_ctx *starfish_ctx_create(struct mp_log *log) {
  auto *ctx = new starfish_ctx();
  ctx->log = mp_log_new(nullptr, log, "starfish");
  if (env_wants_audio_hint()) {
    if (env_wants_pcm_audio()) {
      // PCM route: the load payload's pcmInfo block is parsed by libpf's
      // mediapipeline::setPCMinfo() via rapidjson, reading exactly five
      // fields under option.externalStreamingInfo.contents.pcmInfo:
      // sampleRate, channelMode, format, layout, bitsPerSample.
      // sampleRate is a kHz double matched against an in-binary LUT
      // (48.0/32.0/24.0/16.0/12.0/8.0/22.05); the JSON serializer emits the
      // right form. See STARFISH_PCM_REVERSE_ENGINEERING.md.
      ctx->audio_codec = "PCM";
      ctx->audio_channels = 2;
      ctx->audio_samplerate = 48000;
      ctx->audio_bits_per_sample = 16;
      ctx->audio_pcm_format = "S16LE";
      ctx->audio_pcm_layout = "interleaved";
      ctx->audio_raw = false;
      ctx->need_audio = true;
    } else {
      ctx->audio_codec = "AAC";
      ctx->audio_channels = 2;
      ctx->audio_samplerate = 48000;
      ctx->audio_profile = AV_PROFILE_AAC_LOW;
      ctx->audio_raw = true;
      ctx->need_audio = true;
    }
  }
  ctx->worker = std::thread(worker_loop, ctx);
  return ctx;
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

  {
    std::lock_guard<std::mutex> lk(ctx->lock);
    ctx->stop = true;
  }
  ctx->cv.notify_all();
  if (ctx->worker.joinable())
    ctx->worker.join();

  if (ctx->backend) {
    sf_backend_destroy(ctx->backend);
    ctx->backend = nullptr;
  }
  if (ctx->acb) {
    sf_backend_acb_destroy(ctx->acb);
    ctx->acb = nullptr;
  }
  talloc_free(ctx->log);
  delete ctx;
}

bool starfish_ctx_prime_media(void) { return sf_backend_prime_media(nullptr); }

bool starfish_ctx_unload(struct starfish_ctx *ctx) {
  if (!ctx)
    return true;
  {
    std::unique_lock<std::mutex> lk(ctx->lock);
    ctx->flush_requested = false;
    ctx->eos_pending = false;
    clear_queues_locked(ctx);
    if (is_loaded_state(ctx->state) || ctx->state == pipeline_state::LOADING ||
        ctx->state == pipeline_state::FAILED) {
      lk.unlock();
      if (ctx->backend)
        sf_backend_unload(ctx->backend);
      lk.lock();
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (ctx->state != pipeline_state::IDLE && !ctx->stop) {
        if (ctx->cv.wait_until(lk, deadline) == std::cv_status::timeout)
          break;
      }
      if (ctx->state != pipeline_state::IDLE) {
        mp_warn(ctx->log,
                "Starfish unload: UNLOADCOMPLETED timeout, forcing IDLE\n");
        ctx->state = pipeline_state::IDLE;
      }
    }
    ctx->video_codec.clear();
    ctx->ended = false;
  }
  ctx->cv.notify_all();
  return true;
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
  if (!ctx)
    return;
  set_wakeup(stream == STARFISH_STREAM_VIDEO ? &ctx->video_wakeup
                                             : &ctx->audio_wakeup,
             cb, opaque);
}

void starfish_ctx_set_audio_prime_cb(struct starfish_ctx *ctx,
                                     starfish_audio_prime_cb cb, void *opaque) {
  if (!ctx)
    return;
  set_audio_prime(&ctx->audio_prime, cb, opaque);
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
  return sf_backend_acb_set_display_window(ctx->acb, src_x, src_y, src_w,
                                           src_h, dst_x, dst_y, dst_w, dst_h);
}

bool starfish_ctx_configure_video(struct starfish_ctx *ctx,
                                  const struct mp_codec_params *codec) {
  const char *name =
      video_codec_name((enum AVCodecID)mp_codec_to_av_codec_id(codec->codec));
  if (!name)
    return false;

  // Tear down any previous session so the next Load starts from IDLE.
  starfish_ctx_unload(ctx);

  const AVDOVIDecoderConfigurationRecord *dovi = find_dovi_config(codec);
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (is_loaded_state(ctx->state))
    return false;

  ctx->dovi_mode = get_dovi_policy();
  ctx->video_codec = name;
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

bool starfish_ctx_enable_generated_dovi(struct starfish_ctx *ctx) {
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (is_loaded_state(ctx->state) || ctx->state == pipeline_state::LOADING)
    return ctx->effective_dovi && ctx->dv_profile == 8;
  if (ctx->dovi_mode == dovi_policy::HDR10)
    return false;

  ctx->source_dovi = true;
  ctx->effective_dovi = true;
  ctx->dv_profile = 8;
  ctx->dv_level = 0;
  ctx->dv_rpu_present = true;
  ctx->dv_el_present = false;
  ctx->dv_bl_present = true;
  ctx->dv_bl_signal_compatibility_id = 1;
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
  ctx->audio_samplerate = samplerate;
  ctx->audio_channels = channels ? channels->num : 2;
  ctx->audio_raw = false;
  ctx->need_audio = true;
  return true;
}

bool starfish_ctx_configure_audio_aac(struct starfish_ctx *ctx, int channels,
                                      int samplerate, int profile, bool raw) {
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (is_loaded_state(ctx->state)) {
    const bool same = ctx->need_audio && ctx->audio_codec == "AAC" &&
                      ctx->audio_channels == channels &&
                      ctx->audio_samplerate == samplerate &&
                      ctx->audio_profile == profile && ctx->audio_raw == raw;
    if (!same) {
      mp_warn(ctx->log, "Rejecting Starfish AAC reconfigure while loaded\n");
      return false;
    }
    ctx->audio_queue.clear();
    ctx->audio_queue_bytes = 0;
    ctx->fed_audio_pts_ns = INT64_MIN;
    ctx->audio_bufferfull_logs = 0;
    ctx->cv.notify_all();
    return true;
  }
  ctx->audio_codec = "AAC";
  ctx->audio_channels = channels;
  ctx->audio_samplerate = samplerate;
  ctx->audio_profile = profile;
  ctx->audio_raw = raw;
  ctx->need_audio = true;
  ctx->audio_decode_preroll_ns = 0;
  return true;
}

bool starfish_ctx_configure_audio_pcm(struct starfish_ctx *ctx, int channels,
                                      int samplerate, int bits_per_sample,
                                      const char *pcm_format,
                                      const char *pcm_layout) {
  const std::string format = pcm_format ? pcm_format : "S16LE";
  const std::string layout = pcm_layout ? pcm_layout : "interleaved";
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (is_loaded_state(ctx->state)) {
    const bool same = ctx->need_audio && ctx->audio_codec == "PCM" &&
                      ctx->audio_channels == channels &&
                      ctx->audio_samplerate == samplerate &&
                      ctx->audio_bits_per_sample == bits_per_sample &&
                      ctx->audio_pcm_format == format &&
                      ctx->audio_pcm_layout == layout;
    if (!same) {
      mp_warn(ctx->log, "Rejecting Starfish PCM reconfigure while loaded\n");
      return false;
    }
    ctx->audio_queue.clear();
    ctx->audio_queue_bytes = 0;
    ctx->fed_audio_pts_ns = INT64_MIN;
    ctx->audio_bufferfull_logs = 0;
    ctx->audio_decode_preroll_ns = PCM_DECODE_PREROLL_NS;
    ctx->cv.notify_all();
    return true;
  }
  ctx->audio_codec = "PCM";
  ctx->audio_channels = channels;
  ctx->audio_samplerate = samplerate;
  ctx->audio_bits_per_sample = bits_per_sample;
  ctx->audio_pcm_format = format;
  ctx->audio_pcm_layout = layout;
  ctx->audio_profile = 0;
  ctx->audio_raw = false;
  ctx->need_audio = true;
  ctx->audio_decode_preroll_ns = PCM_DECODE_PREROLL_NS;
  return true;
}

int starfish_ctx_feed_video(struct starfish_ctx *ctx, const void *data,
                            size_t size, double pts, bool keyframe) {
  if (!ctx || !data || size == 0)
    return STARFISH_FEED_ERROR;
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (ctx->state == pipeline_state::FAILED)
    return STARFISH_FEED_ERROR;
  if (!have_load_config(ctx))
    return STARFISH_FEED_ERROR;
  if (ctx->flush_requested)
    return STARFISH_FEED_AGAIN;
  if (ctx->video_queue_bytes + size > VIDEO_QUEUE_LIMIT)
    return STARFISH_FEED_AGAIN;
  const int64_t pts_ns = pts == MP_NOPTS_VALUE ? 0 : (int64_t)(pts * 1e9);

  int64_t anchor_ns = project_fresh_clock_locked(ctx, mp_time_ns());
  if (anchor_ns == INT64_MIN)
    anchor_ns = ctx->current_pts_ns;
  if (anchor_ns != INT64_MIN &&
      pts_ns - anchor_ns > MAX_DECODER_ACCEPT_AHEAD_NS) {
    const int64_t now = mp_time_ns();
    if (!ctx->last_decoder_backpressure_log_ns ||
        now - ctx->last_decoder_backpressure_log_ns >=
            WORKER_STATUS_PERIOD_NS) {
      mp_info(ctx->log,
              "Starfish decoder backpressure packet=%.3f anchor=%.3f "
              "ahead=%.3f queue=%.2fMB/%zu\n",
              (double)pts_ns / 1e9, (double)anchor_ns / 1e9,
              (double)(pts_ns - anchor_ns) / 1e9,
              (double)ctx->video_queue_bytes / (1024.0 * 1024.0),
              ctx->video_queue.size());
      ctx->last_decoder_backpressure_log_ns = now;
    }
    ctx->video_backpressure_pts_ns = pts_ns;
    ctx->cv.notify_all();
    return STARFISH_FEED_AGAIN;
  }

  queued_packet packet;
  packet.data = std::make_shared<std::vector<uint8_t>>(
      (const uint8_t *)data, (const uint8_t *)data + size);
  packet.pts_ns = pts_ns;
  packet.keyframe = keyframe;
  ctx->video_queue_bytes += size;
  ctx->video_queue.push_back(std::move(packet));
  ctx->video_backpressure_pts_ns = INT64_MIN;
  ctx->bufferlow_active = false;
  ctx->cv.notify_all();
  return STARFISH_FEED_OK;
}

int starfish_ctx_feed_audio(struct starfish_ctx *ctx, const void *data,
                            size_t size, int64_t pts_ns) {
  if (!ctx || !data || size == 0)
    return STARFISH_FEED_ERROR;
  std::lock_guard<std::mutex> lk(ctx->lock);
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

bool starfish_ctx_pop_video_frame(struct starfish_ctx *ctx,
                                  struct starfish_video_frame *frame) {
  if (!ctx || !frame)
    return false;
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (!ctx->ready_frames.empty()) {
    *frame = ctx->ready_frames.front();
    ctx->ready_frames.pop_front();
    ctx->current_pts_ns = (int64_t)llround(frame->pts * 1e9);
    ctx->video_clock_base_pts_ns = ctx->current_pts_ns;
    ctx->video_clock_base_host_ns = mp_time_ns();
    return true;
  }
  // Block during pre-roll: only synthesize pacing frames AFTER the first
  // FRAMEREADY has anchored the segment. Returning false here is the natural
  // way to wait — vd_starfish.c keeps the playloop alive via its initial
  // geometry frame (mpv/video/decode/vd_starfish.c:172).
  if (ctx->state != pipeline_state::PLAYING || !ctx->started)
    return false;
  const double fps = ctx->fps > 0.0 ? ctx->fps : 30.0;
  const int64_t frame_ns = (int64_t)llround(1e9 / fps);

  int64_t next_pts_ns = ctx->current_pts_ns + frame_ns;
  const int64_t now = mp_time_ns();
  const int64_t clock_projected = project_fresh_clock_locked(ctx, now);
  if (clock_projected != INT64_MIN) {
    if (clock_projected < ctx->current_pts_ns + frame_ns / 2)
      return false;
    next_pts_ns = clock_projected;
  } else if (ctx->video_clock_base_pts_ns != INT64_MIN &&
      ctx->video_clock_base_host_ns > 0) {
    const int64_t projected =
        ctx->video_clock_base_pts_ns +
        (now - ctx->video_clock_base_host_ns);
    if (projected < ctx->current_pts_ns + frame_ns / 2)
      return false;
    next_pts_ns = projected;
  }
  if (ctx->eos_pushed && ctx->fed_video_pts_ns != INT64_MIN &&
      next_pts_ns > ctx->fed_video_pts_ns + frame_ns)
    return false;
  ctx->current_pts_ns = next_pts_ns;
  ctx->video_clock_base_pts_ns = ctx->current_pts_ns;
  ctx->video_clock_base_host_ns = now;
  frame->pts = (double)ctx->current_pts_ns / 1e9;
  frame->dts = frame->pts;
  frame->duration = 1.0 / fps;
  return true;
}

bool starfish_ctx_resume(struct starfish_ctx *ctx) {
  if (!ctx)
    return false;
  {
    std::lock_guard<std::mutex> lk(ctx->lock);
    if (ctx->started && ctx->current_pts_ns != INT64_MIN) {
      ctx->video_clock_base_pts_ns = ctx->current_pts_ns;
      ctx->video_clock_base_host_ns = mp_time_ns();
    }
    ctx->play_requested = true;
  }
  ctx->cv.notify_all();
  return true;
}

bool starfish_ctx_pause(struct starfish_ctx *ctx) {
  if (!ctx)
    return false;
  {
    std::lock_guard<std::mutex> lk(ctx->lock);
    if (ctx->video_clock_base_pts_ns != INT64_MIN &&
        ctx->video_clock_base_host_ns > 0) {
      ctx->current_pts_ns = ctx->video_clock_base_pts_ns +
                            (mp_time_ns() - ctx->video_clock_base_host_ns);
    }
    ctx->video_clock_base_pts_ns = INT64_MIN;
    ctx->video_clock_base_host_ns = 0;
    ctx->play_requested = false;
  }
  ctx->cv.notify_all();
  return true;
}

bool starfish_ctx_set_seek_target(struct starfish_ctx *ctx, double pts) {
  if (!ctx || pts == MP_NOPTS_VALUE)
    return true;
  std::lock_guard<std::mutex> lk(ctx->lock);
  ctx->pending_seek_target = true;
  ctx->pending_seek_target_ns = (int64_t)(pts * 1e9);
  ctx->seek_target_valid = true;
  ctx->seek_target_ns = ctx->pending_seek_target_ns;
  return true;
}

bool starfish_ctx_flush(struct starfish_ctx *ctx, double pts) {
  if (!ctx)
    return false;
  {
    std::lock_guard<std::mutex> lk(ctx->lock);
    ctx->flush_requested = true;
    if (pts != MP_NOPTS_VALUE) {
      int64_t pts_ns = (int64_t)(pts * 1e9);
      ctx->pending_seek_target = true;
      ctx->pending_seek_target_ns = pts_ns;
      ctx->seek_target_valid = true;
      ctx->seek_target_ns = pts_ns;
      clear_queues_locked(ctx);
      prepare_segment_timeline_locked(ctx, pts_ns, "seek-flush");
      mp_info(ctx->log, "Starfish flush pts set: %" PRId64 "\n", pts_ns);
    } else {
      clear_queues_locked(ctx);
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
  if (!ctx)
    return false;
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
  if (!ctx)
    return true;
  std::lock_guard<std::mutex> lk(ctx->lock);
  return ctx->ended && ctx->ready_frames.empty();
}

bool starfish_ctx_is_failed(struct starfish_ctx *ctx) {
  if (!ctx)
    return true;
  std::lock_guard<std::mutex> lk(ctx->lock);
  return ctx->state == pipeline_state::FAILED;
}

int starfish_ctx_get_video_width(struct starfish_ctx *ctx) {
  if (!ctx)
    return 0;
  std::lock_guard<std::mutex> lk(ctx->lock);
  return ctx->width;
}

int starfish_ctx_get_video_height(struct starfish_ctx *ctx) {
  if (!ctx)
    return 0;
  std::lock_guard<std::mutex> lk(ctx->lock);
  return ctx->height;
}

double starfish_ctx_get_video_fps(struct starfish_ctx *ctx) {
  if (!ctx)
    return 0.0;
  std::lock_guard<std::mutex> lk(ctx->lock);
  return ctx->fps;
}

double starfish_ctx_get_current_pts(struct starfish_ctx *ctx) {
  if (!ctx)
    return 0.0;
  std::lock_guard<std::mutex> lk(ctx->lock);
  const int64_t now = mp_time_ns();
  const int64_t clock_projected = project_fresh_clock_locked(ctx, now);
  if (clock_projected != INT64_MIN)
    return clock_projected / 1e9;

  int64_t projected = INT64_MIN;
  if (ctx->video_clock_base_pts_ns != INT64_MIN &&
      ctx->video_clock_base_host_ns > 0) {
    projected = ctx->video_clock_base_pts_ns +
                (now - ctx->video_clock_base_host_ns);
  }
  if (ctx->eos_pushed && ctx->fed_video_pts_ns != INT64_MIN &&
      projected != INT64_MIN && projected > ctx->fed_video_pts_ns)
    projected = ctx->fed_video_pts_ns;
  if (projected != INT64_MIN && projected > ctx->current_pts_ns)
    return projected / 1e9;
  return ctx->current_pts_ns / 1e9;
}

int starfish_ctx_get_dovi_profile(struct starfish_ctx *ctx) {
  if (!ctx)
    return 0;
  std::lock_guard<std::mutex> lk(ctx->lock);
  return ctx->dv_profile;
}

bool starfish_ctx_get_video_clock(struct starfish_ctx *ctx, double *pts,
                                  int64_t *host_time_ns) {
  if (!ctx || !pts || !host_time_ns)
    return false;
  std::lock_guard<std::mutex> lk(ctx->lock);
  if (ctx->state != pipeline_state::PLAYING || !ctx->started)
    return false;
  if (!ctx->clock_export_ready)
    return false;

  const int64_t now = mp_time_ns();
  if (ctx->clock_sample_valid) {
    const int64_t age = now - ctx->clock_sample_host_ns;
    if (age >= 0 && age <= CLOCK_FRESHNESS_NS) {
      *pts = ctx->clock_sample_pts;
      *host_time_ns = ctx->clock_sample_host_ns;
      return true;
    }
  }
  return false;
}

bool starfish_ctx_get_osd_pts(struct starfish_ctx *ctx, double *pts) {
  if (!ctx || !pts)
    return false;
  std::lock_guard<std::mutex> lk(ctx->lock);

  if (ctx->state == pipeline_state::PAUSED && ctx->started &&
      ctx->current_pts_ns != INT64_MIN) {
    *pts = (double)ctx->current_pts_ns / 1e9;
    return true;
  }

  if (ctx->state != pipeline_state::PLAYING || !ctx->started ||
      !ctx->clock_export_ready)
    return false;

  const int64_t projected = project_fresh_clock_locked(ctx, mp_time_ns());
  if (projected == INT64_MIN)
    return false;

  *pts = (double)projected / 1e9;
  return *pts != MP_NOPTS_VALUE && std::isfinite(*pts);
}

} // extern "C"
