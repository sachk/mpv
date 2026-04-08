#include "starfish_json.h"

#include <cstring>
#include <iomanip>
#include <sstream>

static std::string json_escape(const char *src)
{
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
                out << "\\u"
                    << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned int>(c)
                    << std::dec << std::setfill(' ');
            } else {
                out << static_cast<char>(c);
            }
            break;
        }
    }
    return out.str();
}

std::string starfish_json_build_load(const struct starfish_json_load_params *params)
{
    std::ostringstream out;
    out << "{\"args\":[{"
        << "\"mediaTransportType\":\"BUFFERSTREAM\","
        << "\"option\":{"
        << "\"appId\":\"" << json_escape(params->app_id) << "\","
        << "\"needAudio\":" << (params->need_audio ? "true" : "false") << ','
        << "\"seekMode\":\"keep-rate\","
        << "\"useDroppedFrameEvent\":true,";

    if (params->window_id && params->window_id[0])
        out << "\"windowId\":\"" << json_escape(params->window_id) << "\",";

    out << "\"transmission\":{"
        << "\"contentsType\":\"LIVE\","
        << "\"trickType\":\"client-side\""
        << "},"
        << "\"externalStreamingInfo\":{"
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

    if (params->need_audio && params->audio_codec && strcmp(params->audio_codec, "AAC") == 0) {
        out << ",\"aacInfo\":{"
            << "\"channels\":" << params->audio_channels << ','
            << "\"profile\":" << (params->audio_profile + 1) << ','
            << "\"format\":\"" << (params->audio_raw ? "raw" : "adts") << "\","
            << "\"frequency\":" << std::fixed << std::setprecision(3)
            << (params->audio_samplerate / 1000.0)
            << std::defaultfloat
            << "}";
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
        << "\"preBufferByte\":0,"
        << "\"bufferMinLevel\":0,"
        << "\"bufferMaxLevel\":0,"
        << "\"qBufferLevelVideo\":0,"
        << "\"srcBufferLevelVideo\":{\"minimum\":1048576,\"maximum\":8388608},"
        << "\"qBufferLevelAudio\":0,"
        << "\"srcBufferLevelAudio\":{\"minimum\":1048576,\"maximum\":2097152}"
        << "}"
        << "}";

    if (params->adaptive_resolution &&
        params->max_width > 0 && params->max_height > 0 && params->max_framerate > 0) {
        out << ",\"adaptiveStreaming\":{"
            << "\"adaptiveResolution\":true,"
            << "\"maxWidth\":" << params->max_width << ','
            << "\"maxHeight\":" << params->max_height << ','
            << "\"maxFrameRate\":" << params->max_framerate
            << "}";
    }

    out << "}"
        << "}]}";
    return out.str();
}

std::string starfish_json_build_feed(int es_data, const void *data, size_t size,
                                     int64_t pts_ns)
{
    std::ostringstream out;
    out << "{\"bufferAddr\":\"0x"
        << std::hex << reinterpret_cast<uintptr_t>(data)
        << std::dec << "\","
        << "\"bufferSize\":" << size << ','
        << "\"pts\":" << pts_ns << ','
        << "\"esData\":" << es_data
        << '}';
    return out.str();
}

std::string starfish_json_build_seek(int64_t pts_ns)
{
    std::ostringstream out;
    out << "{\"position\":" << pts_ns << '}';
    return out.str();
}
