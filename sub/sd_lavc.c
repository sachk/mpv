/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <math.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavutil/common.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/opt.h>

#include "common/av_common.h"
#include "common/msg.h"
#include "dec_sub.h"
#include "demux/stheader.h"
#include "img_convert.h"
#include "mpv_talloc.h"
#include "options/options.h"
#include "sd.h"
#include "sub_image_geometry.h"
#include "sub_image_segmentation.h"
#include "sub_recolor.h"
#include "video/mp_image.h"
#include "video/out/bitmap_packer.h"

#define MAX_QUEUE 4

struct sub {
    bool valid;
    AVSubtitle avsub;
    struct sub_bitmap *inbitmaps;
    int count;
    struct mp_image *data;
    int bound_w, bound_h;
    int src_w, src_h;
    // Transparent margin kept around each part for the blur; the ink box of a
    // part is its rectangle shrunk by this on every side.
    int extend;
    double pts;
    double endpts;
    int64_t id;
};

struct seekpoint {
    double pts;
    double endpts;
};

struct sd_lavc_priv {
    struct mp_codec_params *codec;
    AVCodecContext *avctx;
    AVPacket *avpkt;
    AVRational pkt_timebase;
    struct sub subs[MAX_QUEUE]; // most recent event first
    struct sub_bitmap *outbitmaps;
    struct sub_bitmap *prevret;
    int prevret_num;
    int64_t displayed_id;
    int64_t new_id;
    struct mp_image_params video_params;
    double current_pts;
    struct seekpoint *seekpoints;
    int num_seekpoints;
    struct bitmap_packer *packer;

    // DVD-nav menu/highlight overlay state.
    bool menu_active;               // in-menu: gates the highlight overlay
    struct mp_dvdnav_highlight hl;  // focused button rect + palette
    uint32_t hli_change_id;
    // Recoloring happens at decode time, so the queued events have to be
    // rebuilt when these change; this is the copy they were built with.
    struct mp_sub_recolor recolor;
    int *sort_scratch;
    int *block_scratch;
    struct mp_rect *blocks_scratch;
};

static int prepare_all_position_parts(struct sd_lavc_priv *priv, struct sub *current)
{
    int out_count = 0;
    int extend = current->extend;

    for (int i = 0; i < current->count; i++) {
        const struct sub_bitmap *source = &current->inbitmaps[i];
        int ink_width = MPMAX(0, source->w - extend * 2);
        int ink_height = MPMAX(0, source->h - extend * 2);
        int split_gap = MPMAX(extend * 2 + 1, ink_height / 2);
        int max_groups = 1 + ink_width / MPMAX(1, split_gap + 1);
        MP_TARRAY_GROW(priv, priv->outbitmaps, out_count + max_groups);
        MP_TARRAY_GROW(priv, priv->block_scratch, ink_width);
        out_count += mp_image_subtitle_split_columns(
            source, extend, &priv->outbitmaps[out_count], max_groups, priv->block_scratch, ink_width);
    }

    return out_count;
}

static struct mp_sub_recolor recolor_from_opts(struct mp_subtitle_opts *opts)
{
    const struct m_color *c = &opts->sub_image_color;
    const struct m_color *o = &opts->sub_image_outline_color;
    return (struct mp_sub_recolor) {
        .color = c->a << 24 | c->r << 16 | c->g << 8 | c->b,
        .outline_color = o->a << 24 | o->r << 16 | o->g << 8 | o->b,
        .mode = opts->sub_image_color_mode,
        .ink_threshold = opts->sub_image_ink_threshold,
    };
}

static int init(struct sd *sd)
{
    enum AVCodecID cid = mp_codec_to_av_codec_id(sd->codec->codec);

    // Supported codecs must be known to decode to paletted bitmaps
    switch (cid) {
    case AV_CODEC_ID_DVB_SUBTITLE:
    case AV_CODEC_ID_DVB_TELETEXT:
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
    case AV_CODEC_ID_XSUB:
    case AV_CODEC_ID_DVD_SUBTITLE:
    case AV_CODEC_ID_ARIB_CAPTION:
        break;
    default:
        return -1;
    }

    struct sd_lavc_priv *priv = talloc_zero(NULL, struct sd_lavc_priv);
    AVCodecContext *ctx = NULL;
    const AVCodec *sub_codec = avcodec_find_decoder(cid);
    if (!sub_codec)
        goto error_probe;
    ctx = avcodec_alloc_context3(sub_codec);
    if (!ctx)
        goto error_probe;

    mp_set_avopts(sd->log, ctx, sd->opts->sub_avopts);

    switch (cid) {
    case AV_CODEC_ID_DVB_TELETEXT: {
        int64_t format = -1;
        int ret = av_opt_get_int(ctx, "txt_format", AV_OPT_SEARCH_CHILDREN, &format);
        // libzvbi_teletextdec: format == 0 is bitmap
        if (ret || format)
            goto error_probe;
        break;
    }
    case AV_CODEC_ID_ARIB_CAPTION: {
        int64_t format = -1;
        // FFmpeg has both libaribb24 and libaribcaption as decoders. Only the latter
        // can produce bitmaps, so abort if we don't find the option.
        int ret = av_opt_get_int(ctx, "sub_type", AV_OPT_SEARCH_CHILDREN, &format);
        if (ret || format != SUBTITLE_BITMAP)
            goto error_probe;
        break;
    }
    }

    MP_VERBOSE(sd, "Using subtitle decoder %s\n", sub_codec->name);

    priv->avpkt = av_packet_alloc();
    priv->codec = sd->codec;
    if (!priv->avpkt)
        goto error;
    if (mp_set_avctx_codec_headers(ctx, sd->codec) < 0)
        goto error;
    priv->pkt_timebase = mp_get_codec_timebase(sd->codec);
    ctx->pkt_timebase = priv->pkt_timebase;
    if (avcodec_open2(ctx, sub_codec, NULL) < 0)
        goto error;
    priv->avctx = ctx;
    sd->priv = priv;
    priv->displayed_id = -1;
    priv->current_pts = MP_NOPTS_VALUE;
    priv->packer = talloc_zero(priv, struct bitmap_packer);
    return 0;

error:
    MP_FATAL(sd, "Could not open libavcodec subtitle decoder\n");
error_probe:
    avcodec_free_context(&ctx);
    mp_free_av_packet(&priv->avpkt);
    talloc_free(priv);
    return -1;
}

static void clear_sub(struct sub *sub)
{
    sub->count = 0;
    sub->pts = MP_NOPTS_VALUE;
    sub->endpts = MP_NOPTS_VALUE;
    if (sub->valid)
        avsubtitle_free(&sub->avsub);
    sub->valid = false;
}

static void alloc_sub(struct sd_lavc_priv *priv)
{
    clear_sub(&priv->subs[MAX_QUEUE - 1]);
    struct sub tmp = priv->subs[MAX_QUEUE - 1];
    for (int n = MAX_QUEUE - 1; n > 0; n--)
        priv->subs[n] = priv->subs[n - 1];
    priv->subs[0] = tmp;
    // clear only some fields; the memory allocs can be reused
    priv->subs[0].valid = false;
    priv->subs[0].count = 0;
    priv->subs[0].src_w = 0;
    priv->subs[0].src_h = 0;
    priv->subs[0].id = priv->new_id++;
}

static void convert_pal(uint32_t *colors, size_t count, bool gray)
{
    for (int n = 0; n < count; n++) {
        uint32_t c = colors[n];
        uint32_t b = c & 0xFF;
        uint32_t g = (c >> 8) & 0xFF;
        uint32_t r = (c >> 16) & 0xFF;
        uint32_t a = (c >> 24) & 0xFF;
        if (gray)
            r = g = b = (r + g + b) / 3;
        // from straight to pre-multiplied alpha
        b = b * a / 255;
        g = g * a / 255;
        r = r * a / 255;
        colors[n] = b | (g << 8) | (r << 16) | (a << 24);
    }
}

// Initialize sub from sub->avsub.
static void read_sub_bitmaps(struct sd *sd, struct sub *sub)
{
    struct mp_subtitle_opts *opts = sd->opts;
    struct sd_lavc_priv *priv = sd->priv;
    AVSubtitle *avsub = &sub->avsub;

    MP_TARRAY_GROW(priv, sub->inbitmaps, avsub->num_rects);

    packer_set_size(priv->packer, avsub->num_rects);

    // If we blur, we want a transparent region around the bitmap data to
    // avoid "cut off" artifacts on the borders.
    bool apply_blur = opts->sub_gauss != 0.0f;
    int extend = apply_blur ? 5 : 0;
    // Assume consumers may use bilinear scaling on it (2x2 filter)
    int padding = 1 + extend;

    priv->packer->padding = padding;
    sub->extend = extend;
    priv->recolor = recolor_from_opts(opts);

    // For the sake of libswscale, which in some cases takes sub-rects as
    // source images, and wants 16 byte start pointer and stride alignment.
    int align = 4;

    for (int i = 0; i < avsub->num_rects; i++) {
        struct AVSubtitleRect *r = avsub->rects[i];
        struct sub_bitmap *b = &sub->inbitmaps[sub->count];

        if (r->type != SUBTITLE_BITMAP) {
            MP_ERR(sd, "unsupported subtitle type from decoder (%d)\n", r->type);
            continue;
        }
        if (!(r->flags & AV_SUBTITLE_FLAG_FORCED) && opts->sub_forced_events_only)
            continue;
        if (r->w <= 0 || r->h <= 0)
            continue;

        // save the rect for later (dumb hack to avoid more complexity)
        *b = (struct sub_bitmap){ .bitmap = r };

        priv->packer->in[sub->count] = (struct pos) { r->w + (align - 1), r->h };
        sub->count++;
    }

    priv->packer->count = sub->count;

    if (packer_pack(priv->packer) < 0) {
        MP_ERR(sd, "Unable to pack subtitle bitmaps.\n");
        sub->count = 0;
    }

    if (!sub->count)
        return;

    struct pos bb[2];
    packer_get_bb(priv->packer, bb);

    sub->bound_w = bb[1].x;
    sub->bound_h = bb[1].y;

    if (!sub->data || sub->data->w < sub->bound_w || sub->data->h < sub->bound_h) {
        talloc_free(sub->data);
        sub->data = mp_image_alloc(IMGFMT_BGRA, priv->packer->w, priv->packer->h);
        if (!sub->data) {
            sub->count = 0;
            return;
        }
        talloc_steal(priv, sub->data);
    }

    if (!mp_image_make_writeable(sub->data)) {
        sub->count = 0;
        return;
    }

    for (int i = 0; i < sub->count; i++) {
        struct sub_bitmap *b = &sub->inbitmaps[i];
        struct pos pos = priv->packer->result[i];
        struct AVSubtitleRect *r = b->bitmap;
        uint8_t **data = r->data;
        int *linesize = r->linesize;
        b->w = r->w;
        b->h = r->h;
        b->x = r->x;
        b->y = r->y;

        // Choose such that the extended start position is aligned.
        pos.x = MP_ALIGN_UP(pos.x - extend, align) + extend;

        b->src_x = pos.x;
        b->src_y = pos.y;
        b->stride = sub->data->stride[0];
        b->bitmap = sub->data->planes[0] + pos.y * b->stride + pos.x * 4;

        sub->src_w = MPMAX(sub->src_w, b->x + b->w);
        sub->src_h = MPMAX(sub->src_h, b->y + b->h);

        mp_assert(r->nb_colors > 0);
        mp_assert(r->nb_colors <= 256);
        uint32_t pal[256] = { 0 };
        memcpy(pal, data[1], r->nb_colors * 4);

        // One pass over the index plane yields both the pixel occupancy that
        // identifies the glyph fill and the box the text actually occupies.
        // Recoloring never touches alpha, so this classification stays valid
        // across it.
        bool opaque[256];
        for (int n = 0; n < 256; n++)
            opaque[n] = (int)(pal[n] >> 24) > priv->recolor.ink_threshold;

        int counts[256] = { 0 };
        struct mp_rect ink = { b->w, b->h, 0, 0 };
        for (int y = 0; y < b->h; y++) {
            uint8_t *in = data[0] + y * linesize[0];
            int first = -1, last = -1;
            for (int x = 0; x < b->w; x++) {
                counts[in[x]]++;
                if (opaque[in[x]]) {
                    if (first < 0)
                        first = x;
                    last = x;
                }
            }
            if (first < 0)
                continue;
            ink.x0 = MPMIN(ink.x0, first);
            ink.x1 = MPMAX(ink.x1, last + 1);
            ink.y0 = MPMIN(ink.y0, y);
            ink.y1 = MPMAX(ink.y1, y + 1);
        }

        // PGS signals "clear the screen" as a fully transparent rect.
        if (mp_rect_w(ink) <= 0 || mp_rect_h(ink) <= 0) {
            b->w = b->h = 0;
            continue;
        }

        mp_sub_recolor_palette(pal, r->nb_colors, counts, &priv->recolor);
        convert_pal(pal, 256, opts->sub_gray);

        // DVD navigation highlight
        struct mp_rect hlr = priv->hl.rect;
        bool hli_active = priv->menu_active &&
                          mp_rect_w(hlr) > 0 && mp_rect_h(hlr) > 0 &&
                          r->x < hlr.x1 && r->y < hlr.y1 &&
                          r->x + r->w > hlr.x0 && r->y + r->h > hlr.y0;
        uint32_t hli_pal[4] = {0};
        if (hli_active) {
            memcpy(hli_pal, priv->hl.palette, sizeof(hli_pal));
            convert_pal(hli_pal, 4, opts->sub_gray);
        }
        int hli_x0 = hlr.x0 - r->x;
        int hli_y0 = hlr.y0 - r->y;
        int hli_x1 = hlr.x1 - r->x;
        int hli_y1 = hlr.y1 - r->y;

        for (int y = -padding; y < b->h + padding; y++) {
            uint32_t *out = (uint32_t *)((char *)b->bitmap + y * b->stride);
            int start = 0;
            for (int x = -padding; x < 0; x++)
                out[x] = 0;
            if (y >= 0 && y < b->h) {
                uint8_t *in = data[0] + y * linesize[0];
                bool y_in_hli = hli_active && y >= hli_y0 && y < hli_y1;
                for (int x = 0; x < b->w; x++) {
                    uint8_t pv = *in++;
                    if (y_in_hli && x >= hli_x0 && x < hli_x1 && pv < 4)
                        *out++ = hli_pal[pv];
                    else
                        *out++ = pal[pv];
                }
                start = b->w;
            }
            for (int x = start; x < b->w + padding; x++)
                *out++ = 0;
        }

        b->bitmap = (char *)b->bitmap - extend * b->stride - extend * 4;
        b->src_x -= extend;
        b->src_y -= extend;
        b->x -= extend;
        b->y -= extend;
        b->w += extend * 2;
        b->h += extend * 2;

        if (apply_blur)
            mp_blur_rgba_sub_bitmap(b, opts->sub_gauss);

        // Trim the transparent margin the author padded the rect with, so that
        // positioning and scaling act on the text rather than on the bitmap.
        // A sub_bitmap is a view into the packed image, so this is a pointer
        // advance; it shrinks what gets uploaded and blended, not the atlas,
        // which was already packed before this point.
        b->bitmap = (char *)b->bitmap + ink.y0 * b->stride + ink.x0 * 4;
        b->src_x += ink.x0;
        b->src_y += ink.y0;
        b->x += ink.x0;
        b->y += ink.y0;
        b->w = mp_rect_w(ink) + extend * 2;
        b->h = mp_rect_h(ink) + extend * 2;
    }

    // Drop the parts that turned out to hold no ink at all.
    int kept = 0;
    for (int i = 0; i < sub->count; i++) {
        if (sub->inbitmaps[i].w > 0 && sub->inbitmaps[i].h > 0)
            sub->inbitmaps[kept++] = sub->inbitmaps[i];
    }
    sub->count = kept;
}

static void rerender_queued_subs(struct sd *sd)
{
    struct sd_lavc_priv *priv = sd->priv;
    for (int n = 0; n < MAX_QUEUE; n++) {
        struct sub *sub = &priv->subs[n];
        if (!sub->valid)
            continue;
        sub->count = 0;
        sub->src_w = 0;
        sub->src_h = 0;
        sub->id = priv->new_id++;
        read_sub_bitmaps(sd, sub);
    }
}

static void decode(struct sd *sd, struct demux_packet *packet)
{
    struct mp_subtitle_opts *opts = sd->opts;
    struct sd_lavc_priv *priv = sd->priv;
    AVCodecContext *ctx = priv->avctx;
    double pts = packet->pts;
    double endpts = MP_NOPTS_VALUE;
    AVSubtitle sub;

    if (pts == MP_NOPTS_VALUE)
        MP_WARN(sd, "Subtitle with unknown start time.\n");

    mp_set_av_packet(priv->avpkt, packet, &priv->pkt_timebase);

    if (ctx->codec_id == AV_CODEC_ID_DVB_TELETEXT) {
        if (!opts->teletext_page) {
            av_opt_set(ctx, "txt_page", "subtitle", AV_OPT_SEARCH_CHILDREN);
        } else if (opts->teletext_page == -1) {
            av_opt_set(ctx, "txt_page", "*", AV_OPT_SEARCH_CHILDREN);
        } else {
            char page[4];
            snprintf(page, sizeof(page), "%d", opts->teletext_page);
            av_opt_set(ctx, "txt_page", page, AV_OPT_SEARCH_CHILDREN);
        }
    }

    int got_sub;
    int res = avcodec_decode_subtitle2(ctx, &sub, &got_sub, priv->avpkt);
    if (res < 0 || !got_sub)
        return;

    mp_codec_info_from_av(ctx, priv->codec);

    packet->sub_duration = sub.end_display_time;

    if (sub.pts != AV_NOPTS_VALUE)
        pts = sub.pts / (double)AV_TIME_BASE;

    if (pts != MP_NOPTS_VALUE) {
        if (sub.end_display_time > sub.start_display_time && sub.end_display_time != UINT32_MAX) {
            endpts = pts + sub.end_display_time / 1000.0;
        }
        pts += sub.start_display_time / 1000.0;

        // set end time of previous sub
        struct sub *prev = &priv->subs[0];
        if (prev->valid) {
            if (prev->endpts == MP_NOPTS_VALUE || prev->endpts > pts)
                prev->endpts = pts;

            if (opts->sub_fix_timing && pts - prev->endpts <= opts->sub_fix_timing_threshold / 1000.0)
                prev->endpts = pts;

            for (int n = 0; n < priv->num_seekpoints; n++) {
                if (priv->seekpoints[n].pts == prev->pts) {
                    priv->seekpoints[n].endpts = prev->endpts;
                    break;
                }
            }
        }

        // This subtitle packet only signals the end of subtitle display.
        if (!sub.num_rects) {
            avsubtitle_free(&sub);
            return;
        }
    }

    alloc_sub(priv);
    struct sub *current = &priv->subs[0];

    current->valid = true;
    current->pts = pts;
    current->endpts = endpts;
    current->avsub = sub;

    read_sub_bitmaps(sd, current);

    if (pts != MP_NOPTS_VALUE) {
        for (int n = 0; n < priv->num_seekpoints; n++) {
            if (priv->seekpoints[n].pts == pts)
                goto skip;
        }
        // Set arbitrary limit as safe-guard against insane files.
        if (priv->num_seekpoints >= 10000)
            MP_TARRAY_REMOVE_AT(priv->seekpoints, priv->num_seekpoints, 0);
        MP_TARRAY_APPEND(
            priv, priv->seekpoints, priv->num_seekpoints, (struct seekpoint) { .pts = pts, .endpts = endpts });
    skip:;
    }
}

static struct sub *get_current(struct sd_lavc_priv *priv, double pts)
{
    struct sub *current = NULL;
    for (int n = 0; n < MAX_QUEUE; n++) {
        struct sub *sub = &priv->subs[n];
        if (!sub->valid)
            continue;
        if (pts == MP_NOPTS_VALUE
            || ((sub->pts == MP_NOPTS_VALUE || pts + 1e-6 >= sub->pts)
                && (sub->endpts == MP_NOPTS_VALUE || pts + 1e-6 < sub->endpts))) {
            // Ignore "trailing" subtitles with unknown length after 1 minute.
            if (sub->endpts == MP_NOPTS_VALUE && pts >= sub->pts + 60)
                break;
            current = sub;
            break;
        }
    }
    return current;
}

static struct mp_rect part_ink(struct sub_bitmap *b, int extend)
{
    return (struct mp_rect) { b->x + extend, b->y + extend, b->x + b->w - extend, b->y + b->h - extend };
}

static int compare_int(const void *pa, const void *pb)
{
    return *(const int *)pa - *(const int *)pb;
}

// Place image subtitles the way libass places text ones: anchor the bottom of
// the text at --sub-pos, counted within the usable video viewport.
static void reposition_bitmaps(struct sd *sd, struct sub_bitmaps *res, int extend, struct mp_rect vis)
{
    struct sd_lavc_priv *priv = sd->priv;
    struct mp_subtitle_opts *opts = sd->opts;
    float sub_pos = sd->shared_opts->sub_pos[sd->order];
    int n = res->num_parts;

    if (!opts->sub_image_position || n < 1)
        return;
    if (opts->sub_image_position == 2)
        return;

    MP_TARRAY_GROW(priv, priv->sort_scratch, n);
    MP_TARRAY_GROW(priv, priv->block_scratch, n);
    MP_TARRAY_GROW(priv, priv->blocks_scratch, n);
    int *order = priv->sort_scratch;
    int *block_of = priv->block_scratch;
    struct mp_rect *blocks = priv->blocks_scratch;

    // The median line height sets what counts as a break between two separate
    // blocks of text. Unioning every part instead would drag a translated sign
    // at the top of the frame down onto the dialogue.
    for (int i = 0; i < n; i++)
        block_of[i] = mp_rect_h(part_ink(&res->parts[i], extend));
    qsort(block_of, n, sizeof(block_of[0]), compare_int);
    int max_gap = MPMAX(1, block_of[n / 2] * 3 / 2);

    // Sort by the top of the ink. A few parts per event, so insertion sort.
    for (int i = 0; i < n; i++)
        order[i] = i;
    for (int i = 1; i < n; i++) {
        int cur = order[i];
        int y = part_ink(&res->parts[cur], extend).y0;
        int j = i - 1;
        while (j >= 0 && part_ink(&res->parts[order[j]], extend).y0 > y) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = cur;
    }

    int num_blocks = 0;
    int prev_bottom = 0;
    for (int i = 0; i < n; i++) {
        struct mp_rect ink = part_ink(&res->parts[order[i]], extend);
        if (i == 0 || ink.y0 - prev_bottom > max_gap) {
            blocks[num_blocks++] = ink;
            prev_bottom = ink.y1;
        } else {
            struct mp_rect *b = &blocks[num_blocks - 1];
            b->x0 = MPMIN(b->x0, ink.x0);
            b->x1 = MPMAX(b->x1, ink.x1);
            b->y0 = MPMIN(b->y0, ink.y0);
            b->y1 = MPMAX(b->y1, ink.y1);
            prev_bottom = MPMAX(prev_bottom, ink.y1);
        }
        block_of[order[i]] = num_blocks - 1;
    }

    int anchor = num_blocks - 1;
    // bottom-block: only move text that started out near the bottom, and leave
    // signs and other top-of-frame text where the author put them.
    if (opts->sub_image_position == 1 && (blocks[anchor].y0 + blocks[anchor].y1) / 2 < vis.y1 - mp_rect_h(vis) / 3)
        return;

    int target = vis.y1 - lrint(mp_rect_h(vis) * (100.0f - sub_pos) / 100.0);
    int dy = target - blocks[anchor].y1;
    // Never push a block taller than the picture off the top.
    dy = MPMAX(dy, vis.y0 - blocks[anchor].y0);
    int dx = (vis.x0 + vis.x1 - blocks[anchor].x0 - blocks[anchor].x1) / 2;

    for (int i = 0; i < n; i++) {
        if (opts->sub_image_position == 2 || block_of[i] == anchor) {
            res->parts[i].x += dx;
            res->parts[i].y += dy;
        }
    }
}

static struct sub_bitmaps *get_bitmaps(struct sd *sd, struct mp_osd_res d, int format, double pts)
{
    struct sd_lavc_priv *priv = sd->priv;
    struct mp_subtitle_opts *opts = sd->opts;

    priv->current_pts = pts;

    struct sub *current = get_current(priv, pts);

    if (!current)
        return NULL;

    int output_count = 0;
    if (opts->sub_image_position == 2) {
        output_count = prepare_all_position_parts(priv, current);
    } else {
        MP_TARRAY_GROW(priv, priv->outbitmaps, current->count);
        for (int n = 0; n < current->count; n++)
            priv->outbitmaps[n] = current->inbitmaps[n];
        output_count = current->count;
    }

    struct sub_bitmaps *res = &(struct sub_bitmaps) { 0 };
    res->parts = priv->outbitmaps;
    res->num_parts = output_count;
    if (priv->displayed_id != current->id)
        res->change_id++;
    priv->displayed_id = current->id;
    res->packed = current->data;
    res->packed_w = current->bound_w;
    res->packed_h = current->bound_h;
    res->format = SUBBITMAP_BGRA;
    res->video_color_space = true;

    double video_par = 0;
    if (priv->avctx->codec_id == AV_CODEC_ID_DVD_SUBTITLE && opts->stretch_dvd_subs) {
        // For DVD subs, try to keep the subtitle PAR at display PAR.
        double par = priv->video_params.p_w / (double)priv->video_params.p_h;
        if (isnormal(par))
            video_par = par;
    }
    if (priv->avctx->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE) {
        // For Blu-ray subs on SD video, try to match the video PAR.
        if (priv->video_params.w == 720 && (priv->video_params.h == 480 || priv->video_params.h == 576)) {
            double par = priv->video_params.p_w / (double)priv->video_params.p_h;
            if (isnormal(par))
                video_par = par * -1;
            else
                video_par = -1;
        } else {
            // Force letter-boxing on all other Blu-ray subtitles
            video_par = -1;
        }
    }
    if (opts->stretch_image_subs)
        d.ml = d.mr = d.mt = d.mb = 0;
    int w = priv->avctx->width;
    int h = priv->avctx->height;
    if (w <= 0 || h <= 0 || opts->image_subs_video_res) {
        w = priv->video_params.w;
        h = priv->video_params.h;
    }
    if (current->src_w > w || current->src_h > h) {
        w = MPMAX(priv->video_params.w, current->src_w);
        h = MPMAX(priv->video_params.h, current->src_h);
    }

    // Rects are authored in the uncropped frame, so --video-crop has to be
    // resolved here or subtitles written into a letterbox bar get scaled away
    // with the bar. Everything below works in the visible picture.
    struct mp_rect vis = priv->video_params.crop;
    if (mp_rect_w(vis) <= 0 || mp_rect_h(vis) <= 0)
        vis = (struct mp_rect) { 0, 0, w, h };

    reposition_bitmaps(sd, res, current->extend, vis);

    // Fold the crop into the single affine transform osd_rescale_bitmaps()
    // already applies, rather than chaining a second scaling.
    if (mp_rect_w(vis) != w || mp_rect_h(vis) != h) {
        for (int n = 0; n < res->num_parts; n++) {
            res->parts[n].x -= vis.x0;
            res->parts[n].y -= vis.y0;
        }
    }
    osd_rescale_bitmaps(res, mp_rect_w(vis), mp_rect_h(vis), d, video_par);
    struct mp_rect output_visible = mp_image_subtitle_viewport(d, opts->sub_use_margins);
    if (opts->sub_image_position == 2) {
        mp_image_subtitle_reposition_all(res, current->extend, output_visible, sd->shared_opts->sub_pos[sd->order]);
    }

    float scale = sd->shared_opts->sub_scale[sd->order];
    if (scale != 1.0) {
        if (opts->sub_image_position == 2) {
            mp_image_subtitle_scale_all(res, current->extend, scale, output_visible);
        } else {
            for (int n = 0; n < res->num_parts; n++) {
                struct sub_bitmap *sub = &res->parts[n];
                float shit = (scale - 1.0f) / 2;

                // Preserve the historical per-part center scaling unless all
                // authored image geometry is explicitly overridden.
                sub->x -= sub->dw * shit;
                sub->y -= sub->dh * shit;
                sub->dw += sub->dw * shit * 2;
                sub->dh += sub->dh * shit * 2;
            }
        }
    }

    if (priv->prevret_num != res->num_parts)
        res->change_id++;

    if (!res->change_id) {
        mp_assert(priv->prevret_num == res->num_parts);
        for (int n = 0; n < priv->prevret_num; n++) {
            struct sub_bitmap *a = &res->parts[n];
            struct sub_bitmap *b = &priv->prevret[n];

            if (a->x != b->x || a->y != b->y || a->dw != b->dw || a->dh != b->dh) {
                res->change_id++;
                break;
            }
        }
    }

    priv->prevret_num = res->num_parts;
    MP_TARRAY_GROW(priv, priv->prevret, priv->prevret_num);
    memcpy(priv->prevret, res->parts, res->num_parts * sizeof(priv->prevret[0]));

    return sub_bitmaps_copy(NULL, res);
}

static struct sd_times get_times(struct sd *sd, double pts)
{
    struct sd_lavc_priv *priv = sd->priv;
    struct sd_times res = { .start = MP_NOPTS_VALUE, .end = MP_NOPTS_VALUE };

    if (pts == MP_NOPTS_VALUE)
        return res;

    struct sub *current = get_current(priv, pts);

    if (!current)
        return res;

    res.start = current->pts;
    res.end = current->endpts;

    return res;
}

static bool accepts_packet(struct sd *sd, double min_pts)
{
    struct sd_lavc_priv *priv = sd->priv;

    double pts = priv->current_pts;
    if (min_pts != MP_NOPTS_VALUE) {
        // guard against bogus rendering PTS in the future.
        if (pts == MP_NOPTS_VALUE || min_pts < pts)
            pts = min_pts;
        // Heuristic: we assume rendering cannot lag behind more than 1 second
        // behind decoding.
        if (pts + 1 < min_pts)
            pts = min_pts;
    }

    int last_needed = -1;
    for (int n = 0; n < MAX_QUEUE; n++) {
        struct sub *sub = &priv->subs[n];
        if (!sub->valid)
            continue;
        if (pts == MP_NOPTS_VALUE
            || ((sub->pts == MP_NOPTS_VALUE || sub->pts >= pts)
                || (sub->endpts == MP_NOPTS_VALUE || pts < sub->endpts))) {
            last_needed = n;
        }
    }
    // We can accept a packet if it wouldn't overflow the fixed subtitle queue.
    // We assume that get_bitmaps() never decreases the PTS.
    return last_needed + 1 < MAX_QUEUE;
}

static void reset(struct sd *sd)
{
    struct sd_lavc_priv *priv = sd->priv;

    for (int n = 0; n < MAX_QUEUE; n++)
        clear_sub(&priv->subs[n]);
    // lavc might not do this right for all codecs; may need close+reopen
    avcodec_flush_buffers(priv->avctx);

    priv->current_pts = MP_NOPTS_VALUE;
}

static void uninit(struct sd *sd)
{
    struct sd_lavc_priv *priv = sd->priv;

    for (int n = 0; n < MAX_QUEUE; n++)
        clear_sub(&priv->subs[n]);
    avcodec_free_context(&priv->avctx);
    mp_free_av_packet(&priv->avpkt);
    talloc_free(priv);
}

static int compare_seekpoint(const void *pa, const void *pb)
{
    const struct seekpoint *a = pa, *b = pb;
    return a->pts == b->pts ? 0 : (a->pts < b->pts ? -1 : +1);
}

// taken from ass_step_sub(), libass (ISC)
static double step_sub(struct sd *sd, double now, int movement)
{
    struct sd_lavc_priv *priv = sd->priv;
    int best = -1;
    double target = now;
    int direction = (movement > 0 ? 1 : -1) * !!movement;

    if (priv->num_seekpoints == 0)
        return MP_NOPTS_VALUE;

    qsort(priv->seekpoints, priv->num_seekpoints, sizeof(priv->seekpoints[0]), compare_seekpoint);

    do {
        int closest = -1;
        double closest_time = 0;
        for (int i = 0; i < priv->num_seekpoints; i++) {
            struct seekpoint *p = &priv->seekpoints[i];
            double start = p->pts;
            if (direction < 0) {
                double end = p->endpts == MP_NOPTS_VALUE ? INFINITY : p->endpts;
                if (end < target) {
                    if (closest < 0 || end > closest_time) {
                        closest = i;
                        closest_time = end;
                    }
                }
            } else if (direction > 0) {
                if (start > target) {
                    if (closest < 0 || start < closest_time) {
                        closest = i;
                        closest_time = start;
                    }
                }
            } else {
                if (start < target) {
                    if (closest < 0 || start >= closest_time) {
                        closest = i;
                        closest_time = start;
                    }
                }
            }
        }
        if (closest < 0)
            break;
        target = closest_time + direction;
        best = closest;
        movement -= direction;
    } while (movement);

    return best < 0 ? now : priv->seekpoints[best].pts;
}

static int control(struct sd *sd, enum sd_ctrl cmd, void *arg)
{
    struct sd_lavc_priv *priv = sd->priv;
    switch (cmd) {
    case SD_CTRL_SUB_STEP: {
        double *a = arg;
        double res = step_sub(sd, a[0], a[1]);
        if (res == MP_NOPTS_VALUE)
            return false;
        a[0] = res;
        return true;
    }
    case SD_CTRL_UPDATE_OPTS: {
        uint64_t flags = *(uint64_t *)arg;
        if (flags & UPDATE_SUB_ASS_HARD)
            return CONTROL_NA;
        // Recoloring is baked in at decode time, so the queued events have to
        // be expanded again for a color change to show without a seek. Their
        // ids change, which is what makes the VO drop its cached textures.
        struct mp_sub_recolor recolor = recolor_from_opts(sd->opts);
        if (!memcmp(&recolor, &priv->recolor, sizeof(recolor)))
            return CONTROL_OK;
        for (int n = 0; n < MAX_QUEUE; n++) {
            struct sub *sub = &priv->subs[n];
            if (!sub->valid)
                continue;
            sub->count = 0;
            sub->src_w = 0;
            sub->src_h = 0;
            sub->id = priv->new_id++;
            read_sub_bitmaps(sd, sub);
        }
        return CONTROL_OK;
    }
    case SD_CTRL_SET_VIDEO_PARAMS:
        priv->video_params = *(struct mp_image_params *)arg;
        return CONTROL_OK;
    case SD_CTRL_APPLY_DVDNAV: {
        const struct stream_nav_state *nav = arg;
        bool menu_closed = priv->menu_active && !nav->menu_active;
        bool changed = priv->hli_change_id != nav->change_id;
        priv->menu_active = nav->menu_active;
        priv->hl = nav->hl;
        priv->hli_change_id = nav->change_id;
        // Don't let menu subpictures leak into title playback.
        if (menu_closed) {
            for (int n = 0; n < MAX_QUEUE; n++)
                clear_sub(&priv->subs[n]);
        }
        // Re-render any decoded subtitles, after style update.
        if (changed)
            rerender_queued_subs(sd);
        return CONTROL_OK;
    }
    default:
        return CONTROL_UNKNOWN;
    }
}

const struct sd_functions sd_lavc = {
    .name = "lavc",
    .init = init,
    .decode = decode,
    .get_bitmaps = get_bitmaps,
    .get_times = get_times,
    .accepts_packet = accepts_packet,
    .control = control,
    .reset = reset,
    .uninit = uninit,
};
