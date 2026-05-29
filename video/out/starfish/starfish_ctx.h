#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mp_codec_params;
struct mp_chmap;
struct mp_hwdec_ctx;
struct mp_log;

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define STARFISH_CTX_API __attribute__((visibility("default")))
#else
#define STARFISH_CTX_API
#endif

struct starfish_ctx;

enum starfish_stream_type {
  STARFISH_STREAM_VIDEO = 1,
  STARFISH_STREAM_AUDIO = 2,
};

enum starfish_feed_status {
  STARFISH_FEED_ERROR = -1,
  STARFISH_FEED_OK = 0,
  STARFISH_FEED_AGAIN = 1,
};

struct starfish_video_frame {
  double pts;
  double dts;
  double duration;
};

typedef void (*starfish_wakeup_cb)(void *opaque);
typedef bool (*starfish_audio_prime_cb)(void *opaque, int64_t pts_ns);
typedef void (*starfish_overlay_present_cb)(void *opaque, const uint8_t *pixels,
                                            int width, int height, int stride);
typedef void (*starfish_exported_crop_cb)(void *opaque, int orig_w, int orig_h,
                                          int src_x, int src_y, int src_w,
                                          int src_h, int dst_x, int dst_y,
                                          int dst_w, int dst_h);

STARFISH_CTX_API struct starfish_ctx *starfish_ctx_create(struct mp_log *log);
STARFISH_CTX_API struct starfish_ctx *
starfish_ctx_retain(struct starfish_ctx *ctx);
STARFISH_CTX_API void starfish_ctx_unref(struct starfish_ctx *ctx);
STARFISH_CTX_API bool starfish_ctx_prime_media(void);
STARFISH_CTX_API bool starfish_ctx_unload(struct starfish_ctx *ctx);

STARFISH_CTX_API struct starfish_ctx *
starfish_ctx_from_hwdec(struct mp_hwdec_ctx *hwctx);

STARFISH_CTX_API bool starfish_ctx_set_current(struct starfish_ctx *ctx);
STARFISH_CTX_API struct starfish_ctx *starfish_ctx_get_current(void);
STARFISH_CTX_API void
starfish_overlay_set_present_cb(starfish_overlay_present_cb cb, void *opaque);
STARFISH_CTX_API void
starfish_exported_set_crop_cb(starfish_exported_crop_cb cb, void *opaque);

STARFISH_CTX_API void
starfish_ctx_set_wakeup_cb(struct starfish_ctx *ctx,
                           enum starfish_stream_type stream,
                           starfish_wakeup_cb cb, void *opaque);
STARFISH_CTX_API void
starfish_ctx_set_audio_prime_cb(struct starfish_ctx *ctx,
                                starfish_audio_prime_cb cb, void *opaque);

STARFISH_CTX_API bool starfish_ctx_set_window_id(struct starfish_ctx *ctx,
                                                 const char *window_id);
STARFISH_CTX_API bool
starfish_ctx_set_numeric_window_id(struct starfish_ctx *ctx, int64_t wid);
STARFISH_CTX_API bool starfish_ctx_set_video_geometry(struct starfish_ctx *ctx,
                                                      int width, int height,
                                                      double fps);
STARFISH_CTX_API bool starfish_ctx_set_display_window(struct starfish_ctx *ctx,
                                                      int src_x, int src_y,
                                                      int src_w, int src_h,
                                                      int dst_x, int dst_y,
                                                      int dst_w, int dst_h);
STARFISH_CTX_API bool
starfish_ctx_configure_video(struct starfish_ctx *ctx,
                             const struct mp_codec_params *codec);
STARFISH_CTX_API bool
starfish_ctx_enable_generated_dovi(struct starfish_ctx *ctx);
STARFISH_CTX_API bool
starfish_ctx_configure_audio_passthrough(struct starfish_ctx *ctx, int format,
                                          int samplerate,
                                          const struct mp_chmap *channels);
STARFISH_CTX_API bool starfish_ctx_configure_audio_aac(struct starfish_ctx *ctx,
                                                       int channels,
                                                       int samplerate,
                                                       int profile, bool raw);
// Configure uncompressed-PCM audio fed as an elementary stream (esData=2),
// independent of the AAC path. pcm_format is a gstreamer sample-format token
// (e.g. "S16LE"); pcm_layout is "interleaved" or "non-interleaved".
STARFISH_CTX_API bool
starfish_ctx_configure_audio_pcm(struct starfish_ctx *ctx, int channels,
                                 int samplerate, int bits_per_sample,
                                 const char *pcm_format,
                                 const char *pcm_layout);

STARFISH_CTX_API int starfish_ctx_feed_video(struct starfish_ctx *ctx,
                                             const void *data, size_t size,
                                             double pts, bool keyframe);
STARFISH_CTX_API int starfish_ctx_feed_audio(struct starfish_ctx *ctx,
                                             const void *data, size_t size,
                                             int64_t pts_ns);
STARFISH_CTX_API bool
starfish_ctx_pop_video_frame(struct starfish_ctx *ctx,
                             struct starfish_video_frame *frame);
STARFISH_CTX_API bool starfish_ctx_resume(struct starfish_ctx *ctx);
STARFISH_CTX_API bool starfish_ctx_pause(struct starfish_ctx *ctx);
STARFISH_CTX_API bool starfish_ctx_set_seek_target(struct starfish_ctx *ctx,
                                                   double pts);
STARFISH_CTX_API bool starfish_ctx_flush(struct starfish_ctx *ctx, double pts);
STARFISH_CTX_API bool starfish_ctx_get_seek_target_ns(struct starfish_ctx *ctx,
                                                      int64_t *pts_ns);
STARFISH_CTX_API bool
starfish_ctx_get_audio_reset_target_ns(struct starfish_ctx *ctx,
                                       int64_t *pts_ns,
                                       bool *needs_segment_prime);
STARFISH_CTX_API bool starfish_ctx_push_eos(struct starfish_ctx *ctx);
STARFISH_CTX_API bool starfish_ctx_has_ended(struct starfish_ctx *ctx);
STARFISH_CTX_API bool starfish_ctx_is_failed(struct starfish_ctx *ctx);

STARFISH_CTX_API int starfish_ctx_get_video_width(struct starfish_ctx *ctx);
STARFISH_CTX_API int starfish_ctx_get_video_height(struct starfish_ctx *ctx);
STARFISH_CTX_API double starfish_ctx_get_video_fps(struct starfish_ctx *ctx);
STARFISH_CTX_API double starfish_ctx_get_current_pts(struct starfish_ctx *ctx);
STARFISH_CTX_API int starfish_ctx_get_dovi_profile(struct starfish_ctx *ctx);

// Real Starfish video-master clock. The session worker samples
// StarfishMediaAPIs::getCurrentPlaytime() periodically; this returns the most
// recent valid sample without blocking on the SDK. Returns false during
// pre-roll or while paused (when no recent sample exists).
STARFISH_CTX_API bool starfish_ctx_get_video_clock(struct starfish_ctx *ctx,
                                                   double *pts,
                                                   int64_t *host_time_ns);

#ifdef __cplusplus
}
#endif
