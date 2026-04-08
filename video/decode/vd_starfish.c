/*
 * This file is part of mpv.
 */

#include <stdbool.h>
#include <math.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/hwcontext.h>

#include "common/av_common.h"
#include "common/codecs.h"
#include "common/common.h"
#include "common/msg.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "filters/f_decoder_wrapper.h"
#include "filters/filter_internal.h"
#include "mpv_talloc.h"
#include "video/hwdec.h"
#include "video/img_format.h"
#include "video/mp_image.h"
#include "video/out/starfish/starfish_ctx.h"

struct priv {
    struct mp_log *log;
    struct mp_codec_params *codec;
    struct starfish_ctx *ctx;
    AVBSFContext *bsf;
    AVPacket *avpkt;
    AVPacket *filtered_pkt;
    struct demux_packet *pending;
    bool have_filtered;
    bool input_eof;
    bool sent_eof;
    struct mp_decoder public;
};

static struct starfish_ctx *get_ctx(struct mp_filter *parent)
{
    struct mp_stream_info *info = mp_filter_find_stream_info(parent);
    if (!info || !info->hwdec_devs)
        return starfish_ctx_get_current();

    struct mp_hwdec_ctx *hwctx =
        hwdec_devices_get_by_imgfmt_and_type(info->hwdec_devs, IMGFMT_STARFISH,
                                             AV_HWDEVICE_TYPE_NONE);
    if (hwctx)
        return starfish_ctx_from_hwdec(hwctx);

    return starfish_ctx_get_current();
}

static int init_bsf(struct priv *p)
{
    const char *name = NULL;

    switch (mp_codec_to_av_codec_id(p->codec->codec)) {
    case AV_CODEC_ID_H264:
        name = "h264_mp4toannexb";
        break;
    case AV_CODEC_ID_HEVC:
        name = "hevc_mp4toannexb";
        break;
    default:
        return 0;
    }

    MP_INFO(p, "vd_starfish init_bsf codec=%s extradata_size=%d lav_extradata_size=%d\n",
            p->codec->codec ? p->codec->codec : "(null)",
            p->codec->extradata_size,
            p->codec->lav_codecpar ? p->codec->lav_codecpar->extradata_size : -1);

    if (!p->codec->lav_codecpar || p->codec->lav_codecpar->extradata_size <= 0)
        return 0;

    const AVBitStreamFilter *filter = av_bsf_get_by_name(name);
    if (!filter) {
        MP_WARN(p, "Bitstream filter %s not available; feeding packets as-is\n", name);
        return 0;
    }

    if (av_bsf_alloc(filter, &p->bsf) < 0)
        return -1;
    if (avcodec_parameters_copy(p->bsf->par_in, p->codec->lav_codecpar) < 0)
        return -1;
    p->bsf->time_base_in = (AVRational){1, 1000000};
    if (av_bsf_init(p->bsf) < 0)
        return -1;

    p->avpkt = av_packet_alloc();
    p->filtered_pkt = av_packet_alloc();
    MP_INFO(p, "vd_starfish enabled bitstream filter %s\n", name);
    return p->avpkt && p->filtered_pkt ? 0 : -1;
}

static void clear_pending(struct priv *p)
{
    if (p->pending) {
        talloc_free(p->pending);
        p->pending = NULL;
    }
    p->have_filtered = false;
    if (p->filtered_pkt)
        av_packet_unref(p->filtered_pkt);
}

static bool prepare_filtered_packet(struct priv *p)
{
    if (!p->pending || !p->bsf || p->have_filtered)
        return true;

    av_packet_unref(p->avpkt);
    if (av_new_packet(p->avpkt, p->pending->len) < 0) {
        MP_ERR(p, "Failed to allocate packet for bitstream filter input\n");
        return false;
    }
    memcpy(p->avpkt->data, p->pending->buffer, p->pending->len);
    p->avpkt->size = p->pending->len;
    p->avpkt->pts = p->pending->pts == MP_NOPTS_VALUE ? AV_NOPTS_VALUE
                                                      : llrint(p->pending->pts * 1000000.0);
    p->avpkt->dts = p->pending->dts == MP_NOPTS_VALUE ? AV_NOPTS_VALUE
                                                      : llrint(p->pending->dts * 1000000.0);
    if (p->pending->keyframe)
        p->avpkt->flags |= AV_PKT_FLAG_KEY;

    if (av_bsf_send_packet(p->bsf, p->avpkt) < 0) {
        MP_ERR(p, "Failed to send packet to bitstream filter\n");
        return false;
    }
    if (av_bsf_receive_packet(p->bsf, p->filtered_pkt) < 0) {
        MP_ERR(p, "Failed to receive filtered bitstream packet\n");
        return false;
    }

    p->have_filtered = true;
    return true;
}

static void output_ready_frame(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (!mp_pin_in_needs_data(f->ppins[1]))
        return;

    struct starfish_video_frame frame;
    if (!starfish_ctx_pop_video_frame(p->ctx, &frame))
        return;

    struct mp_image *mpi = mp_image_new_dummy_ref(NULL);
    if (!mpi) {
        mp_filter_internal_mark_failed(f);
        return;
    }

    mp_image_setfmt(mpi, IMGFMT_STARFISH);
    mp_image_set_size(mpi, starfish_ctx_get_video_width(p->ctx),
                      starfish_ctx_get_video_height(p->ctx));
    mpi->pts = frame.pts;
    mpi->dts = frame.dts;
    mpi->pkt_duration = frame.duration;
    mpi->nominal_fps = starfish_ctx_get_video_fps(p->ctx);

    mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, mpi));
}

static void maybe_output_eof(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (!p->input_eof || p->sent_eof || !mp_pin_in_needs_data(f->ppins[1]))
        return;
    if (!starfish_ctx_has_ended(p->ctx))
        return;

    p->sent_eof = true;
    mp_pin_in_write(f->ppins[1], MP_EOF_FRAME);
}

static bool feed_pending(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (!p->pending)
        return false;

    const void *data = p->pending->buffer;
    size_t size = p->pending->len;
    if (p->bsf) {
        if (!prepare_filtered_packet(p)) {
            mp_filter_internal_mark_failed(f);
            return false;
        }
        data = p->filtered_pkt->data;
        size = p->filtered_pkt->size;
    }

    MP_INFO(p, "vd_starfish feeding bytes=%02x %02x %02x %02x size=%zu filtered=%d\n",
            size > 0 ? ((const unsigned char *)data)[0] : 0,
            size > 1 ? ((const unsigned char *)data)[1] : 0,
            size > 2 ? ((const unsigned char *)data)[2] : 0,
            size > 3 ? ((const unsigned char *)data)[3] : 0,
            size, p->bsf ? 1 : 0);

    int r = starfish_ctx_feed_video(p->ctx, data, size, p->pending->pts);
    MP_INFO(p, "vd_starfish feed_pending size=%zu pts=%f status=%d\n",
            size, p->pending->pts, r);
    if (r == STARFISH_FEED_OK) {
        clear_pending(p);
        return true;
    }
    if (r == STARFISH_FEED_ERROR)
        mp_filter_internal_mark_failed(f);
    return false;
}

static void process_input(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (p->pending || p->input_eof)
        return;
    if (!mp_pin_out_request_data(f->ppins[0]))
        return;

    struct mp_frame frame = mp_pin_out_read(f->ppins[0]);
    if (frame.type == MP_FRAME_EOF) {
        p->input_eof = true;
        if (!starfish_ctx_push_eos(p->ctx))
            MP_WARN(p, "Failed to push Starfish EOS\n");
        return;
    }
    if (frame.type == MP_FRAME_NONE)
        return;
    if (frame.type != MP_FRAME_PACKET) {
        mp_frame_unref(&frame);
        mp_filter_internal_mark_failed(f);
        return;
    }

    p->pending = frame.data;
    MP_INFO(p, "vd_starfish queued packet size=%zu pts=%f dts=%f\n",
            p->pending->len, p->pending->pts, p->pending->dts);
}

static int control(struct mp_filter *f, enum dec_ctrl cmd, void *arg)
{
    struct priv *p = f->priv;

    switch (cmd) {
    case VDCTRL_REINIT:
        starfish_ctx_flush(p->ctx, 0);
        return CONTROL_TRUE;
    case VDCTRL_GET_HWDEC:
        *(char **)arg = "starfish";
        return CONTROL_TRUE;
    default:
        return CONTROL_UNKNOWN;
    }
}

static void vd_starfish_process(struct mp_filter *f)
{
    process_input(f);
    output_ready_frame(f);
    maybe_output_eof(f);
    if (feed_pending(f))
        mp_filter_internal_mark_progress(f);
}

static void vd_starfish_reset(struct mp_filter *f)
{
    struct priv *p = f->priv;

    clear_pending(p);
    p->input_eof = false;
    p->sent_eof = false;
    if (p->bsf)
        av_bsf_flush(p->bsf);
    starfish_ctx_flush(p->ctx, 0);
}

static void vd_starfish_destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;

    clear_pending(p);
    av_packet_free(&p->filtered_pkt);
    av_packet_free(&p->avpkt);
    av_bsf_free(&p->bsf);
    starfish_ctx_set_wakeup_cb(p->ctx, STARFISH_STREAM_VIDEO, NULL, NULL);
    starfish_ctx_unref(p->ctx);
}

static void wake_decoder(void *opaque)
{
    struct mp_filter *f = opaque;
    MP_INFO(f, "vd_starfish wake_decoder\n");
    mp_filter_wakeup(f);
}

static const struct mp_filter_info vd_starfish_filter = {
    .name = "vd_starfish",
    .priv_size = sizeof(struct priv),
    .process = vd_starfish_process,
    .reset = vd_starfish_reset,
    .destroy = vd_starfish_destroy,
};

static struct mp_decoder *create(struct mp_filter *parent,
                                 struct mp_codec_params *codec,
                                 const char *decoder)
{
    struct starfish_ctx *ctx = get_ctx(parent);
    if (!ctx)
        return NULL;

    struct mp_filter *vd = mp_filter_create(parent, &vd_starfish_filter);
    if (!vd) {
        starfish_ctx_unref(ctx);
        return NULL;
    }

    mp_filter_add_pin(vd, MP_PIN_IN, "in");
    mp_filter_add_pin(vd, MP_PIN_OUT, "out");
    vd->log = mp_log_new(vd, parent->log, NULL);

    struct priv *p = vd->priv;
    p->log = vd->log;
    p->codec = codec;
    p->ctx = ctx;
    p->public.f = vd;
    p->public.control = control;

    starfish_ctx_set_wakeup_cb(p->ctx, STARFISH_STREAM_VIDEO, wake_decoder, vd);
    MP_INFO(vd, "vd_starfish create codec=%s decoder=%s\n",
            codec->codec ? codec->codec : "(null)", decoder ? decoder : "(null)");
    if (!starfish_ctx_configure_video(p->ctx, codec)) {
        talloc_free(vd);
        return NULL;
    }
    if (init_bsf(p) < 0) {
        talloc_free(vd);
        return NULL;
    }

    return &p->public;
}

static void add_decoders(struct mp_decoder_list *list)
{
    mp_add_decoder(list, "h264", "starfish", "LG webOS Starfish packet sink");
    mp_add_decoder(list, "hevc", "starfish", "LG webOS Starfish packet sink");
    mp_add_decoder(list, "vp9", "starfish", "LG webOS Starfish packet sink");
    mp_add_decoder(list, "av1", "starfish", "LG webOS Starfish packet sink");
}

const struct mp_decoder_fns vd_starfish = {
    .create = create,
    .add_decoders = add_decoders,
};
