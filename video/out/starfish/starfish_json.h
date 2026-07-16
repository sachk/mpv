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
  bool dolby_vision;
  int dolby_vision_profile;
  bool dolby_vision_dual_layer;
  int audio_channels;
  int audio_profile;
  int audio_samplerate;
  bool audio_raw;
  int width;
  int height;
  int fps_num;
  int fps_den;
  int max_width;
  int max_height;
  int max_framerate;
  bool adaptive_resolution;
  int64_t pts_to_decode_ns;
  bool need_audio;
  bool audio_sync;
  /* PCM audio (audio_codec == "PCM"); ignored for other codecs.
   * audio_pcm_format is a libpf format token from the setPCMinfo string
   *   table: "S16LE"/"S16BE"/"U16LE"/"U16BE"/"S24LE"/"S24BE"/"U24LE"/"U24BE"/
   *   "S24_32LE"/"S24_32BE"/"U24_32LE"/"U24_32BE"/"S32LE"/"S32BE"/"U32LE"/
   *   "U32BE"/"S20LE"/"S20BE"/"U20LE"/"U20BE"/"S18LE"/"S18BE"/"U18LE"/"U18BE"/
   *   "F32LE"/"F32BE"/"F64LE"/"F64BE"/"S8"/"U8".
   * audio_pcm_layout is "interleaved" or "non-interleaved"; the only two
   *   strings libpf accepts. (Not "planar".) */
  int audio_bits_per_sample;
  const char *audio_pcm_format;
  const char *audio_pcm_layout;
};

struct starfish_json_hdr_info_params {
  const char *hdr_type;
  bool has_sei;
  int display_primaries_x0;
  int display_primaries_y0;
  int display_primaries_x1;
  int display_primaries_y1;
  int display_primaries_x2;
  int display_primaries_y2;
  int white_point_x;
  int white_point_y;
  int min_display_mastering_luminance;
  int max_display_mastering_luminance;
  int max_content_light_level;
  int max_pic_average_light_level;
  int transfer_characteristics;
  int color_primaries;
  int matrix_coeffs;
  bool video_full_range_flag;
};

std::string
starfish_json_build_load(const struct starfish_json_load_params *params);
std::string starfish_json_build_hdr_info(
    const struct starfish_json_hdr_info_params *params);
