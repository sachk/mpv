#pragma once

/*
 * starfish_backend: narrow wrapper over the proprietary LG webOS Starfish
 * C++ SDK (StarfishMediaAPIs, CustomPipeline, AcbAPI).
 *
 * Only starfish_backend.cpp includes the LG SDK headers. Every other file in
 * the mpv tree can pull in starfish_backend.h without dragging in C++
 * proprietary types.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mp_log;

enum sf_backend_stream {
    SF_BACKEND_VIDEO = 1,
    SF_BACKEND_AUDIO = 2,
};

enum sf_backend_feed_result {
    SF_BACKEND_FEED_ERROR = -1,
    SF_BACKEND_FEED_OK = 0,
    SF_BACKEND_FEED_BUFFER_FULL = 1,
    SF_BACKEND_FEED_RETRY = 2,
};

enum sf_backend_event_type {
    SF_EVENT_UNKNOWN = 0,
    SF_EVENT_FRAME_READY,             // num_value = raw frame pts (ns)
    SF_EVENT_PRELOAD_COMPLETED,
    SF_EVENT_LOAD_COMPLETED,
    SF_EVENT_UNLOAD_COMPLETED,
    SF_EVENT_PLAYING,
    SF_EVENT_PAUSED,
    SF_EVENT_SEEK_DONE,
    SF_EVENT_END_OF_STREAM,
    SF_EVENT_BUFFER_LOW,
    SF_EVENT_BUFFER_FULL,
    SF_EVENT_VIDEO_INFO,              // str_value = JSON payload
    SF_EVENT_AUDIO_INFO,              // str_value = JSON payload
    SF_EVENT_DROPPED_FRAME,           // one event per frame dropped by Starfish
    SF_EVENT_ERROR,
};

struct sf_backend_load_params {
    const char *payload_json;
};

struct sf_backend_packet {
    enum sf_backend_stream stream;
    const void *data;
    size_t size;
    int64_t pts_ns;
};

struct sf_backend_clock_sample {
    bool valid;
    double pts;                       // playback seconds
    int64_t host_time_ns;             // midpoint of the SDK call
    int64_t query_duration_ns;        // duration of the SDK call
};

struct sf_backend_callbacks {
    void (*event)(void *opaque, enum sf_backend_event_type type,
                  int64_t num_value, const char *str_value);
    void *opaque;
};

struct sf_backend;

struct sf_backend *sf_backend_create(struct mp_log *log,
                                     const struct sf_backend_callbacks *callbacks);
void sf_backend_destroy(struct sf_backend *b);

bool sf_backend_notify_foreground(struct sf_backend *b);
bool sf_backend_load(struct sf_backend *b,
                     const struct sf_backend_load_params *params);
bool sf_backend_unload(struct sf_backend *b);
bool sf_backend_play(struct sf_backend *b);
bool sf_backend_pause(struct sf_backend *b);
bool sf_backend_flush(struct sf_backend *b);
bool sf_backend_push_eos(struct sf_backend *b);
bool sf_backend_set_time_to_decode(struct sf_backend *b, int64_t pts_ns);
bool sf_backend_send_segment_event(struct sf_backend *b);
bool sf_backend_set_play_rate(struct sf_backend *b, int play_rate_millis,
                              bool audio_output);
bool sf_backend_set_hdr_info(struct sf_backend *b, const char *payload_json);
enum sf_backend_feed_result sf_backend_feed(struct sf_backend *b,
                                            const struct sf_backend_packet *packet);
bool sf_backend_get_current_playtime(struct sf_backend *b,
                                     struct sf_backend_clock_sample *sample);
const char *sf_backend_get_media_id(struct sf_backend *b);

// Warm allocation. Subsequent sf_backend_create() reuses the primed instance.
bool sf_backend_prime_media(struct mp_log *log);

// Opaque ACB handle (private webOS API for window/state callbacks). Only used
// when no external window_id was provided to the session.
struct sf_backend_acb;

struct sf_backend_acb *sf_backend_acb_create(struct mp_log *log,
                                             const char *app_id);
void sf_backend_acb_destroy(struct sf_backend_acb *acb);
bool sf_backend_acb_set_display_window(struct sf_backend_acb *acb,
                                       int src_x, int src_y, int src_w, int src_h,
                                       int dst_x, int dst_y, int dst_w, int dst_h);
void sf_backend_acb_attach(struct sf_backend_acb *acb, const char *media_id);
void sf_backend_acb_set_play_state(struct sf_backend_acb *acb, int play_state);
void sf_backend_acb_set_video_info(struct sf_backend_acb *acb,
                                   const char *payload);

enum sf_backend_acb_state {
    SF_ACB_LOADED = 0,
    SF_ACB_PLAYING = 1,
    SF_ACB_PAUSED = 2,
    SF_ACB_UNLOADED = 3,
};
