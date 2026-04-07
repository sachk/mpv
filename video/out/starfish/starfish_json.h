#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string>

struct starfish_json_load_params {
    const char *app_id;
    const char *window_id;
    const char *video_codec;
    const char *audio_codec;
    int width;
    int height;
    int fps_num;
    int fps_den;
    int64_t pts_to_decode_ns;
    bool need_audio;
};

std::string starfish_json_build_load(const struct starfish_json_load_params *params);
std::string starfish_json_build_feed(int es_data, const void *data, size_t size,
                                     int64_t pts_ns);
std::string starfish_json_build_seek(int64_t pts_ns);
