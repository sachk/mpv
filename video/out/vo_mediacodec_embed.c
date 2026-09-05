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

#include <libavcodec/mediacodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>

#include <pthread.h>
#include <string.h>

#include "common/common.h"
#include "common/msg.h"
#include "options/m_option.h"
#include "sub/draw_bmp.h"
#include "sub/img_convert.h"
#include "sub/osd.h"
#include "vo.h"
#include "video/mp_image.h"
#include "video/hwdec.h"
#include "video/out/android_overlay.h"
#include "video/out/aspect.h"

struct priv {
    struct mp_image *next_image;
    struct mp_hwdec_ctx hwctx;

    // The window this plane is embedded in, in physical pixels. MediaCodec
    // decides nothing about placement -- the embedder sizes the surface to the
    // frame and centres it -- so the only thing this output needs the geometry
    // for is knowing where subtitles go, black bars included.
    int configured_window_width;
    int configured_window_height;

    struct mp_osd_res osd;
    struct mp_draw_sub_cache *draw_cache;
    int64_t osd_change_id;
    struct mp_osd_res osd_res_drawn;
    bool osd_state_valid;
    double last_osd_pts;
};

// Set once by the embedder before playback, and read from the VO thread.
static pthread_mutex_t overlay_lock = PTHREAD_MUTEX_INITIALIZER;
static android_overlay_acquire_cb overlay_acquire_cb;
static android_overlay_present_cb overlay_present_cb;
static void *overlay_opaque;

void android_overlay_set_callbacks(android_overlay_acquire_cb acquire_cb,
                                   android_overlay_present_cb present_cb,
                                   void *opaque)
{
    pthread_mutex_lock(&overlay_lock);
    overlay_acquire_cb = acquire_cb;
    overlay_present_cb = present_cb;
    overlay_opaque = opaque;
    pthread_mutex_unlock(&overlay_lock);
}

// Trim the rendered OSD to the pixels that are actually in it. A film with one
// line of dialogue at the bottom otherwise costs a full-window image every time
// the line changes, and that image crosses to the embedder.
static bool crop_osd_list(struct sub_bitmap_list *osd, int width, int height,
                          int *x, int *y)
{
    struct mp_rect bounds = {width, height, 0, 0};
    bool have_bounds = false;

    for (int n = 0; n < osd->num_items; n++) {
        struct mp_rect item;
        if (!mp_sub_bitmaps_bb(osd->items[n], &item))
            continue;
        bounds.x0 = MPMIN(bounds.x0, item.x0);
        bounds.y0 = MPMIN(bounds.y0, item.y0);
        bounds.x1 = MPMAX(bounds.x1, item.x1);
        bounds.y1 = MPMAX(bounds.y1, item.y1);
        have_bounds = true;
    }

    bounds.x0 = MPCLAMP(bounds.x0, 0, width);
    bounds.y0 = MPCLAMP(bounds.y0, 0, height);
    bounds.x1 = MPCLAMP(bounds.x1, bounds.x0, width);
    bounds.y1 = MPCLAMP(bounds.y1, bounds.y0, height);
    if (!have_bounds || bounds.x0 >= bounds.x1 || bounds.y0 >= bounds.y1)
        return false;

    for (int n = 0; n < osd->num_items; n++) {
        struct sub_bitmaps *item = osd->items[n];
        for (int i = 0; i < item->num_parts; i++) {
            item->parts[i].x -= bounds.x0;
            item->parts[i].y -= bounds.y0;
        }
    }

    *x = bounds.x0;
    *y = bounds.y0;
    osd->w = bounds.x1 - bounds.x0;
    osd->h = bounds.y1 - bounds.y0;
    return true;
}

static void render_osd(struct vo *vo, double pts)
{
    struct priv *p = vo->priv;

    pthread_mutex_lock(&overlay_lock);
    android_overlay_acquire_cb acquire = overlay_acquire_cb;
    android_overlay_present_cb present = overlay_present_cb;
    void *opaque = overlay_opaque;
    pthread_mutex_unlock(&overlay_lock);

    if (!acquire || !present || p->osd.w <= 0 || p->osd.h <= 0)
        return;

    p->last_osd_pts = pts;

    struct sub_bitmap_list *osd =
        osd_render(vo->osd, p->osd, pts, 0, mp_draw_sub_formats);
    int overlay_x = 0;
    int overlay_y = 0;
    const bool has_pixels = osd->num_items > 0 &&
                            crop_osd_list(osd, p->osd.w, p->osd.h,
                                          &overlay_x, &overlay_y);
    // osd_render answers with the same change id for as long as nothing has
    // moved, which is most frames of most films.
    if (p->osd_state_valid && p->osd_change_id == osd->change_id &&
        osd_res_equals(p->osd_res_drawn, p->osd))
    {
        talloc_free(osd);
        return;
    }
    p->osd_change_id = osd->change_id;
    p->osd_res_drawn = p->osd;
    p->osd_state_valid = true;

    if (!has_pixels) {
        present(opaque, NULL, false);
        talloc_free(osd);
        return;
    }

    int stride = 0;
    void *buffer = NULL;
    uint8_t *pixels = acquire(opaque, overlay_x, overlay_y, osd->w, osd->h,
                              &stride, &buffer);
    if (!pixels || !buffer || stride < osd->w * 4) {
        if (buffer)
            present(opaque, buffer, false);
        talloc_free(osd);
        return;
    }

    struct mp_image mpi = {0};
    mp_image_setfmt(&mpi, IMGFMT_BGRA);
    mp_image_set_size(&mpi, osd->w, osd->h);
    mpi.params.repr.alpha = PL_ALPHA_PREMULTIPLIED;
    mpi.planes[0] = pixels;
    mpi.stride[0] = stride;
    memset(mpi.planes[0], 0, (size_t)mpi.h * mpi.stride[0]);

    if (!p->draw_cache)
        p->draw_cache = mp_draw_sub_alloc(p, vo->global);
    if (!mp_draw_sub_bitmaps(p->draw_cache, &mpi, osd)) {
        present(opaque, buffer, false);
        talloc_free(osd);
        return;
    }

    talloc_free(osd);
    present(opaque, buffer, true);
}

static void clear_osd(struct vo *vo)
{
    struct priv *p = vo->priv;
    pthread_mutex_lock(&overlay_lock);
    android_overlay_present_cb present = overlay_present_cb;
    void *opaque = overlay_opaque;
    pthread_mutex_unlock(&overlay_lock);
    p->osd_state_valid = false;
    if (present)
        present(opaque, NULL, false);
}

static AVBufferRef *create_mediacodec_device_ref(struct vo *vo)
{
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!device_ref)
        return NULL;

    AVHWDeviceContext *ctx = (void *)device_ref->data;
    AVMediaCodecDeviceContext *hwctx = ctx->hwctx;
    mp_assert(vo->opts->WinID != 0 && vo->opts->WinID != -1);
    hwctx->surface = (void *)(intptr_t)(vo->opts->WinID);

    if (av_hwdevice_ctx_init(device_ref) < 0)
        av_buffer_unref(&device_ref);

    return device_ref;
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    vo->hwdec_devs = hwdec_devices_create();
    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = "mediacodec_embed",
        .av_device_ref = create_mediacodec_device_ref(vo),
        .hw_imgfmt = IMGFMT_MEDIACODEC,
    };

    if (!p->hwctx.av_device_ref) {
        MP_VERBOSE(vo, "Failed to create hwdevice_ctx\n");
        return -1;
    }

    hwdec_devices_add(vo->hwdec_devs, &p->hwctx);
    return 0;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (!p->next_image)
        return;

    AVMediaCodecBuffer *buffer = (AVMediaCodecBuffer *)p->next_image->planes[3];
    av_mediacodec_release_buffer(buffer, 1);
    mp_image_unrefp(&p->next_image);
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;

    mp_image_t *mpi = NULL;
    if (!frame->redraw && !frame->repeat)
        mpi = mp_image_new_ref(frame->current);

    talloc_free(p->next_image);
    p->next_image = mpi;

    // The OSD is not on the video plane and so is not bound to the video's
    // cadence, but this is the one place with a presentation time to draw it
    // against.
    render_osd(vo, frame->current ? frame->current->pts : MP_NOPTS_VALUE);
    return VO_TRUE;
}

static int query_format(struct vo *vo, int format)
{
    return format == IMGFMT_MEDIACODEC;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    switch (request) {
    case VOCTRL_REDRAW:
        // VO_CAP_NORETAIN means there is no frame kept to redraw, and none is
        // needed: the video plane holds its last frame by itself. What a
        // redraw is actually for here is the OSD, which changes while a paused
        // picture does not.
        render_osd(vo, vo->priv ? ((struct priv *)vo->priv)->last_osd_pts
                                : MP_NOPTS_VALUE);
        return VO_TRUE;
    case VOCTRL_RESET:
        clear_osd(vo);
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;

    if (p->configured_window_width > 0 && p->configured_window_height > 0) {
        vo->dwidth = p->configured_window_width;
        vo->dheight = p->configured_window_height;
    }

    struct mp_rect src, dst;
    vo_get_src_dst_rects(vo, &src, &dst, &p->osd);
    osd_resize(vo->osd, p->osd);
    p->osd_state_valid = false;

    MP_VERBOSE(vo, "MediaCodec geometry window=%dx%d video=%dx%d osd=%dx%d "
                   "margins=%d,%d,%d,%d\n",
               vo->dwidth, vo->dheight, params ? params->w : 0,
               params ? params->h : 0, p->osd.w, p->osd.h,
               p->osd.mt, p->osd.mb, p->osd.ml, p->osd.mr);
    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    clear_osd(vo);
    TA_FREEP(&p->draw_cache);
    mp_image_unrefp(&p->next_image);

    hwdec_devices_remove(vo->hwdec_devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);
}

#define OPT_BASE_STRUCT struct priv

const struct vo_driver video_out_mediacodec_embed = {
    .description = "Android (Embedded MediaCodec Surface)",
    .name = "mediacodec_embed",
    .caps = VO_CAP_NORETAIN,
    .preinit = preinit,
    .query_format = query_format,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .reconfig = reconfig,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
    .options = (const struct m_option[]) {
        {"window-width", OPT_INT(configured_window_width), M_RANGE(0, 16384)},
        {"window-height", OPT_INT(configured_window_height), M_RANGE(0, 16384)},
        {0}
    },
    .options_prefix = "vo-mediacodec-embed",
};
