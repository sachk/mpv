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

struct starfish_ctx *starfish_ctx_create(struct mp_log *log);
struct starfish_ctx *starfish_ctx_retain(struct starfish_ctx *ctx);
void starfish_ctx_unref(struct starfish_ctx *ctx);

struct starfish_ctx *starfish_ctx_from_hwdec(struct mp_hwdec_ctx *hwctx);

bool starfish_ctx_set_current(struct starfish_ctx *ctx);
struct starfish_ctx *starfish_ctx_get_current(void);

void starfish_ctx_set_wakeup_cb(struct starfish_ctx *ctx, enum starfish_stream_type stream,
                                starfish_wakeup_cb cb, void *opaque);

bool starfish_ctx_set_window_id(struct starfish_ctx *ctx, const char *window_id);
bool starfish_ctx_set_numeric_window_id(struct starfish_ctx *ctx, int64_t wid);
bool starfish_ctx_set_video_geometry(struct starfish_ctx *ctx, int width, int height,
                                     double fps);
bool starfish_ctx_set_display_window(struct starfish_ctx *ctx,
                                     int src_x, int src_y, int src_w, int src_h,
                                     int dst_x, int dst_y, int dst_w, int dst_h);
bool starfish_ctx_configure_video(struct starfish_ctx *ctx,
                                  const struct mp_codec_params *codec);
bool starfish_ctx_configure_audio_passthrough(struct starfish_ctx *ctx, int format,
                                              int samplerate,
                                              const struct mp_chmap *channels);

int starfish_ctx_feed_video(struct starfish_ctx *ctx, const void *data, size_t size,
                            double pts);
int starfish_ctx_feed_audio(struct starfish_ctx *ctx, const void *data, size_t size,
                            int64_t pts_ns);
bool starfish_ctx_pop_video_frame(struct starfish_ctx *ctx,
                                  struct starfish_video_frame *frame);
bool starfish_ctx_resume(struct starfish_ctx *ctx);
bool starfish_ctx_pause(struct starfish_ctx *ctx);
bool starfish_ctx_flush(struct starfish_ctx *ctx, double pts);
bool starfish_ctx_push_eos(struct starfish_ctx *ctx);
bool starfish_ctx_has_ended(struct starfish_ctx *ctx);

int starfish_ctx_get_video_width(struct starfish_ctx *ctx);
int starfish_ctx_get_video_height(struct starfish_ctx *ctx);
double starfish_ctx_get_video_fps(struct starfish_ctx *ctx);

#ifdef __cplusplus
}
#endif
