/*
 * This file is part of mpv.
 */

#include <stdlib.h>
#include <string.h>

#include "common/common.h"
#include "common/msg.h"
#include "present_sync.h"
#include "sub/osd.h"
#include "video/hwdec.h"
#include "video/mp_image.h"
#include "video/out/vo.h"
#include "video/out/starfish/starfish_ctx.h"
#include "video/out/wayland_common.h"
#include "webos-foreign.h"

struct priv {
    struct mp_image *next_image;
    struct mp_hwdec_ctx hwctx;
    struct starfish_ctx *ctx;
    struct wl_webos_exported *exported;
    char *window_id;
    bool window_ready;
    bool exported_path;
    struct mp_rect last_src;
    struct mp_rect last_dst;
    int last_w;
    int last_h;
};

static void exported_window_id_assigned(void *data,
                                        struct wl_webos_exported *exported,
                                        const char *window_id,
                                        uint32_t exported_type)
{
    struct vo *vo = data;
    struct priv *p = vo->priv;

    MP_INFO(vo, "wl_webos_exported window_id_assigned=%s type=%u\n",
            window_id ? window_id : "(null)", exported_type);
    talloc_free(p->window_id);
    p->window_id = talloc_strdup(NULL, window_id ? window_id : "");
    p->window_ready = p->window_id && p->window_id[0];
    if (p->window_ready)
        starfish_ctx_set_window_id(p->ctx, p->window_id);
    vo_wakeup(vo);
}

static const struct wl_webos_exported_listener exported_listener = {
    .window_id_assigned = exported_window_id_assigned,
};

static void set_exported_crop(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct vo_wayland_state *wl = vo->wl;

    if (!p->exported || !p->window_ready || !vo->params ||
        !wl || !wl->compositor || vo->dwidth <= 0 || vo->dheight <= 0)
        return;

    struct mp_rect src, dst;
    struct mp_osd_res osd;
    vo_get_src_dst_rects(vo, &src, &dst, &osd);
    if (mp_rect_equals(&src, &p->last_src) &&
        mp_rect_equals(&dst, &p->last_dst) &&
        p->last_w == vo->params->w && p->last_h == vo->params->h)
        return;

    struct wl_region *orig = wl_compositor_create_region(wl->compositor);
    struct wl_region *src_region = wl_compositor_create_region(wl->compositor);
    struct wl_region *dst_region = wl_compositor_create_region(wl->compositor);

    wl_region_add(orig, 0, 0, vo->params->w, vo->params->h);
    wl_region_add(src_region, src.x0, src.y0, mp_rect_w(src), mp_rect_h(src));
    wl_region_add(dst_region, dst.x0, dst.y0, mp_rect_w(dst), mp_rect_h(dst));
    wl_webos_exported_set_crop_region(p->exported, orig, src_region, dst_region);
    wl_region_destroy(orig);
    wl_region_destroy(src_region);
    wl_region_destroy(dst_region);

    p->last_src = src;
    p->last_dst = dst;
    p->last_w = vo->params->w;
    p->last_h = vo->params->h;

    MP_VERBOSE(vo, "Updated Starfish exported crop: src=%d,%d %dx%d dst=%d,%d %dx%d\n",
               src.x0, src.y0, mp_rect_w(src), mp_rect_h(src),
               dst.x0, dst.y0, mp_rect_w(dst), mp_rect_h(dst));
}

static int resize(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct vo_wayland_state *wl = vo->wl;

    if (!wl)
        return VO_TRUE;

    const int32_t width = mp_rect_w(wl->geometry);
    const int32_t height = mp_rect_h(wl->geometry);
    if (width <= 0 || height <= 0)
        return VO_TRUE;

    vo->dwidth = width;
    vo->dheight = height;
    vo_wayland_set_opaque_region(wl, false);
    set_exported_crop(vo);

    if (!p->exported_path && vo->params) {
        starfish_ctx_set_display_window(p->ctx, 0, 0, vo->params->w, vo->params->h,
                                        0, 0, width, height);
    }

    vo->want_redraw = true;
    return VO_TRUE;
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    const char *window_id = getenv("STARFISH_WINDOW_ID");
    bool external_window = (window_id && window_id[0]) || vo->opts->WinID > 0;

    p->ctx = starfish_ctx_create(vo->log);
    if (!p->ctx)
        return -1;

    if (!external_window && !vo_wayland_init(vo))
        goto err;

    vo->hwdec_devs = hwdec_devices_create();
    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = "starfish",
        .hw_imgfmt = IMGFMT_STARFISH,
        .conversion_config = p->ctx,
    };

    if (window_id && window_id[0]) {
        p->window_ready = true;
        p->window_id = talloc_strdup(NULL, window_id);
        starfish_ctx_set_window_id(p->ctx, window_id);
    } else if (vo->opts->WinID > 0) {
        p->window_ready = true;
        starfish_ctx_set_numeric_window_id(p->ctx, vo->opts->WinID);
    } else if (vo->wl && vo->wl->webos_foreign) {
        p->exported = wl_webos_foreign_export_element(
            vo->wl->webos_foreign, vo->wl->video_surface,
            WL_WEBOS_FOREIGN_WEBOS_EXPORTED_TYPE_VIDEO_OBJECT);
        if (!p->exported) {
            MP_FATAL(vo, "wl_webos_foreign_export_element failed\n");
            goto err;
        }
        wl_webos_exported_add_listener(p->exported, &exported_listener, vo);
        wl_display_roundtrip(vo->wl->display);
        p->exported_path = true;
    } else {
        MP_WARN(vo, "webOS exported window unavailable, using ACB fallback\n");
    }

    hwdec_devices_add(vo->hwdec_devs, &p->hwctx);
    starfish_ctx_set_current(p->ctx);
    return 0;

err:
    if (vo->hwdec_devs) {
        hwdec_devices_destroy(vo->hwdec_devs);
        vo->hwdec_devs = NULL;
    }
    if (p->ctx) {
        starfish_ctx_unref(p->ctx);
        p->ctx = NULL;
    }
    if (vo->wl)
        vo_wayland_uninit(vo);
    return -1;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;

    mp_image_unrefp(&p->next_image);
    if (vo->wl && vo->wl->use_present)
        present_sync_swap(vo->wl->present);
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;

    mp_image_unrefp(&p->next_image);
    if (frame->current && !frame->redraw && !frame->repeat)
        p->next_image = mp_image_new_ref(frame->current);
    set_exported_crop(vo);
    return VO_TRUE;
}

static int query_format(struct vo *vo, int format)
{
    return format == IMGFMT_STARFISH;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;

    switch (request) {
    case VOCTRL_RESET:
        return starfish_ctx_flush(p->ctx, 0) ? VO_TRUE : VO_ERROR;
    case VOCTRL_PAUSE:
        return starfish_ctx_pause(p->ctx) ? VO_TRUE : VO_ERROR;
    case VOCTRL_RESUME:
        return starfish_ctx_resume(p->ctx) ? VO_TRUE : VO_ERROR;
    case VOCTRL_SET_PANSCAN:
        return resize(vo);
    }

    int events = 0;
    int ret = vo->wl ? vo_wayland_control(vo, &events, request, data) : VO_NOTIMPL;
    if (events & VO_EVENT_RESIZE)
        ret = resize(vo);
    if (events & VO_EVENT_EXPOSE) {
        vo->want_redraw = true;
        set_exported_crop(vo);
    }
    vo_event(vo, events);
    return ret;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;

    if (vo->wl && !vo_wayland_reconfig(vo))
        return -1;
    starfish_ctx_set_video_geometry(p->ctx, params->w, params->h, 0);
    if (!vo->wl)
        return 0;
    return resize(vo) < 0 ? -1 : 0;
}

static void get_vsync(struct vo *vo, struct vo_vsync_info *info)
{
    struct vo_wayland_state *wl = vo->wl;
    if (wl && wl->use_present)
        present_sync_get_info(wl->present, info);
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;

    mp_image_unrefp(&p->next_image);
    talloc_free(p->window_id);
    if (p->exported)
        wl_webos_exported_destroy(p->exported);
    starfish_ctx_set_current(NULL);
    if (vo->hwdec_devs) {
        hwdec_devices_remove(vo->hwdec_devs, &p->hwctx);
        hwdec_devices_destroy(vo->hwdec_devs);
        vo->hwdec_devs = NULL;
    }
    starfish_ctx_unref(p->ctx);
    if (vo->wl)
        vo_wayland_uninit(vo);
}

static void wakeup(struct vo *vo)
{
    if (vo->wl)
        vo_wayland_wakeup(vo);
}

static void wait_events(struct vo *vo, int64_t until_time_ns)
{
    if (vo->wl)
        vo_wayland_wait_events(vo, until_time_ns);
}

const struct vo_driver video_out_starfish = {
    .description = "LG webOS Starfish",
    .name = "starfish",
    .caps = VO_CAP_NORETAIN,
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .get_vsync = get_vsync,
    .wakeup = wakeup,
    .wait_events = wait_events,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
};
