#include "starfish_json.h"

#include <cstring>
#include <iomanip>
#include <sstream>

namespace {

constexpr unsigned int PRE_BUFFER_BYTES = 0;
constexpr unsigned int MAX_QUEUE_BUFFER_LEVEL = 0;
constexpr unsigned int MIN_BUFFER_LEVEL = 0;
constexpr unsigned int MAX_BUFFER_LEVEL = 0;
constexpr unsigned int MIN_SRC_BUFFER_LEVEL_AUDIO = 1 * 1024 * 1024;
constexpr unsigned int MIN_SRC_BUFFER_LEVEL_VIDEO = 1 * 1024 * 1024;
constexpr unsigned int MAX_SRC_BUFFER_LEVEL_AUDIO = 2 * 1024 * 1024;
constexpr unsigned int MAX_SRC_BUFFER_LEVEL_VIDEO = 8 * 1024 * 1024;

} // namespace

static std::string json_escape(const char *src) {
  if (!src)
    return "";

  std::ostringstream out;
  while (*src) {
    const unsigned char c = *src++;
    switch (c) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (c < 0x20) {
        out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<unsigned int>(c) << std::dec << std::setfill(' ');
      } else {
        out << static_cast<char>(c);
      }
      break;
    }
  }
  return out.str();
}

std::string
starfish_json_build_load(const struct starfish_json_load_params *params) {
  std::ostringstream out;
  out << "{\"args\":[{"
      << "\"mediaTransportType\":\"BUFFERSTREAM\","
      << "\"option\":{"
      << "\"appId\":\"" << json_escape(params->app_id) << "\","
      << "\"needAudio\":" << (params->need_audio ? "true" : "false") << ','
      << "\"seekMode\":\"keep-rate\","
      << "\"queryPosition\":true,"
      << "\"useCurrentTimeWithSystemClock\":true,"
      << "\"useDroppedFrameEvent\":true,";

  if (params->window_id && params->window_id[0])
    out << "\"windowId\":\"" << json_escape(params->window_id) << "\",";

  out << "\"transmission\":{"
      << "\"contentsType\":\"LIVE\","
      << "\"trickType\":\"client-side\""
      << "},"
      << "\"externalStreamingInfo\":{"
      << "\"audioSync\":" << (params->audio_sync ? "true" : "false") << ","
      << "\"streamQualityInfo\":true,"
      << "\"streamQualityInfoNonFlushable\":true,"
      << "\"streamQualityInfoCorruptedFrame\":true,"
      << "\"contents\":{"
      << "\"format\":\"RAW\","
      << "\"provider\":\"" << json_escape(params->app_id) << "\","
      << "\"codec\":{"
      << "\"video\":\"" << json_escape(params->video_codec) << "\"";

  if (params->need_audio && params->audio_codec && params->audio_codec[0])
    out << ",\"audio\":\"" << json_escape(params->audio_codec) << "\"";

  out << "}";

  if (params->dolby_vision) {
    out << ",\"DolbyHdrInfo\":{"
        << "\"encryptionType\":\"clear\","
        << "\"profileId\":" << params->dolby_vision_profile << ','
        << "\"trackType\":\""
        << (params->dolby_vision_dual_layer ? "dual" : "single") << "\"";
    out << "}";
  }

  if (params->need_audio && params->audio_codec &&
      strcmp(params->audio_codec, "AAC") == 0) {
    out << ",\"aacInfo\":{"
        << "\"channels\":" << params->audio_channels << ','
        << "\"profile\":" << (params->audio_profile + 1) << ','
        << "\"format\":\"" << (params->audio_raw ? "raw" : "adts") << "\","
        << "\"frequency\":" << std::fixed << std::setprecision(3)
        << (params->audio_samplerate / 1000.0) << std::defaultfloat << "}";
  }

  if (params->need_audio && params->audio_codec &&
      strcmp(params->audio_codec, "PCM") == 0) {
    out << ",\"pcmInfo\":{"
        << "\"channels\":" << params->audio_channels << ','
        << "\"channelMode\":\""
        << (params->audio_channels == 1 ? "mono" : "stereo") << "\","
        << "\"sampleRate\":" << params->audio_samplerate << ','
        << "\"bitsPerSample\":" << params->audio_bits_per_sample << ','
        << "\"format\":\""
        << json_escape(params->audio_pcm_format ? params->audio_pcm_format
                                                : "S16LE")
        << "\",\"layout\":\"interleaved\"}";
  }

  out << ",\"esInfo\":{"
      << "\"pauseAtDecodeTime\":true,"
      << "\"seperatedPTS\":true,"
      << "\"ptsToDecode\":" << params->pts_to_decode_ns << ','
      << "\"videoWidth\":" << params->width << ','
      << "\"videoHeight\":" << params->height;

  if (params->fps_num > 0 && params->fps_den > 0) {
    out << ",\"videoFpsValue\":" << params->fps_num
        << ",\"videoFpsScale\":" << params->fps_den;
  }

  out << "}"
      << "},"
      << "\"bufferingCtrInfo\":{"
      << "\"preBufferByte\":" << PRE_BUFFER_BYTES << ','
      << "\"bufferMinLevel\":" << MIN_BUFFER_LEVEL << ','
      << "\"bufferMaxLevel\":" << MAX_BUFFER_LEVEL << ','
      << "\"qBufferLevelVideo\":" << MAX_QUEUE_BUFFER_LEVEL << ','
      << "\"srcBufferLevelVideo\":{\"minimum\":"
      << MIN_SRC_BUFFER_LEVEL_VIDEO << ",\"maximum\":"
      << MAX_SRC_BUFFER_LEVEL_VIDEO << "},"
      << "\"qBufferLevelAudio\":" << MAX_QUEUE_BUFFER_LEVEL << ','
      << "\"srcBufferLevelAudio\":{\"minimum\":"
      << MIN_SRC_BUFFER_LEVEL_AUDIO << ",\"maximum\":"
      << MAX_SRC_BUFFER_LEVEL_AUDIO << "}"
      << "}"
      << "}";

  if (params->adaptive_resolution && params->max_width > 0 &&
      params->max_height > 0 && params->max_framerate > 0) {
    out << ",\"adaptiveStreaming\":{"
        << "\"adaptiveResolution\":true,"
        << "\"maxWidth\":" << params->max_width << ','
        << "\"maxHeight\":" << params->max_height << ','
        << "\"maxFrameRate\":" << params->max_framerate << "}";
  }

  out << "}"
      << "}]}";
  return out.str();
}

std::string starfish_json_build_hdr_info(
    const struct starfish_json_hdr_info_params *params) {
  std::ostringstream out;
  out << '{' << "\"hdrType\":\""
      << json_escape(params->hdr_type ? params->hdr_type : "none") << "\","
      << "\"sei\":{"
      << "\"displayPrimariesX0\":" << params->display_primaries_x0 << ','
      << "\"displayPrimariesY0\":" << params->display_primaries_y0 << ','
      << "\"displayPrimariesX1\":" << params->display_primaries_x1 << ','
      << "\"displayPrimariesY1\":" << params->display_primaries_y1 << ','
      << "\"displayPrimariesX2\":" << params->display_primaries_x2 << ','
      << "\"displayPrimariesY2\":" << params->display_primaries_y2 << ','
      << "\"whitePointX\":" << params->white_point_x << ','
      << "\"whitePointY\":" << params->white_point_y << ','
      << "\"minDisplayMasteringLuminance\":"
      << params->min_display_mastering_luminance << ','
      << "\"maxDisplayMasteringLuminance\":"
      << params->max_display_mastering_luminance << ','
      << "\"maxContentLightLevel\":" << params->max_content_light_level << ','
      << "\"maxPicAverageLightLevel\":" << params->max_pic_average_light_level
      << "},"
      << "\"vui\":{"
      << "\"transferCharacteristics\":" << params->transfer_characteristics
      << ',' << "\"colorPrimaries\":" << params->color_primaries << ','
      << "\"matrixCoeffs\":" << params->matrix_coeffs << ','
      << "\"videoFullRangeFlag\":"
      << (params->video_full_range_flag ? "true" : "false") << "}" << '}';
  return out.str();
}

std::string starfish_json_build_feed(int es_data, const void *data, size_t size,
                                     int64_t pts_ns) {
  std::ostringstream out;
  out << "{\"bufferAddr\":\"0x" << std::hex << reinterpret_cast<uintptr_t>(data)
      << std::dec << "\","
      << "\"bufferSize\":" << size << ',' << "\"pts\":" << pts_ns << ','
      << "\"esData\":" << es_data << '}';
  return out.str();
}

std::string starfish_json_build_seek(int64_t pts_ns) {
  std::ostringstream out;
  out << "{\"position\":" << pts_ns << '}';
  return out.str();
}

std::string starfish_json_build_play_rate(int play_rate_millis,
                                          bool audio_output) {
  std::ostringstream out;
  out << "{\"audioOutput\":" << (audio_output ? "true" : "false")
      << ",\"playRate\":" << std::fixed << std::setprecision(3)
      << (play_rate_millis / 1000.0) << std::defaultfloat << '}';
  return out.str();
}
