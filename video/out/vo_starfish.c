/*
 * This file is part of mpv.
 */

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common/common.h"
#include "common/msg.h"
#include "osdep/endian.h"
#include "present_sync.h"
#include "sub/osd.h"
#include "video/hwdec.h"
#include "video/mp_image.h"
#include "video/out/vo.h"
#include "video/out/starfish/starfish_ctx.h"
#include "video/out/wayland_common.h"
#include "webos-foreign.h"
#include "single-pixel-buffer-v1.h"
#include "viewporter.h"

struct buffer {
    struct vo *vo;
    size_t size;
    struct wl_shm_pool *pool;
    struct wl_buffer *buffer;
    struct mp_image mpi;
    struct buffer *next;
};

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
    struct buffer *free_buffers;
    struct mp_osd_res osd;
    struct wl_shm_pool *solid_buffer_pool;
    struct wl_buffer *solid_buffer;
    uint8_t *callback_pixels;
    size_t callback_size;
    int callback_stride;
    struct mp_image_params target_params;
    bool logged_osd_pixels;
    bool logged_osd_skip;
    bool logged_draw_frame;
    bool logged_resize;
};

static pthread_mutex_t overlay_cb_lock = PTHREAD_MUTEX_INITIALIZER;
static starfish_overlay_present_cb overlay_present_cb;
static void *overlay_present_opaque;

STARFISH_CTX_API void starfish_overlay_set_present_cb(starfish_overlay_present_cb cb,
                                                      void *opaque)
{
    pthread_mutex_lock(&overlay_cb_lock);
    overlay_present_cb = cb;
    overlay_present_opaque = opaque;
    pthread_mutex_unlock(&overlay_cb_lock);
}

static void buffer_handle_release(void *data, struct wl_buffer *wl_buffer)
{
    struct buffer *buf = data;
    struct vo *vo = buf->vo;
    struct priv *p = vo->priv;

    if (buf->mpi.w == vo->dwidth && buf->mpi.h == vo->dheight) {
        buf->next = p->free_buffers;
        p->free_buffers = buf;
    } else {
        talloc_free(buf);
    }
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_handle_release,
};

static void buffer_destroy(void *ptr)
{
    struct buffer *buf = ptr;
    if (buf->buffer)
        wl_buffer_destroy(buf->buffer);
    if (buf->pool)
        wl_shm_pool_destroy(buf->pool);
    if (buf->mpi.planes[0])
        munmap(buf->mpi.planes[0], buf->size);
}

static struct buffer *buffer_create(struct vo *vo, int width, int height)
{
    struct vo_wayland_state *wl = vo->wl;
    int stride = MP_ALIGN_UP(width * 4, MP_IMAGE_BYTE_ALIGN);
    size_t size = (size_t)height * stride;
    int fd = vo_wayland_allocate_memfd(vo, size);
    if (fd < 0)
        return NULL;

    uint8_t *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    struct buffer *buf = talloc_zero(NULL, struct buffer);
    if (!buf) {
        munmap(data, size);
        close(fd);
        return NULL;
    }

    buf->vo = vo;
    buf->size = size;
    mp_image_setfmt(&buf->mpi, IMGFMT_BGRA);
    mp_image_set_size(&buf->mpi, width, height);
    buf->mpi.params.repr.alpha = PL_ALPHA_PREMULTIPLIED;
    buf->mpi.planes[0] = data;
    buf->mpi.stride[0] = stride;
    buf->pool = wl_shm_create_pool(wl->shm, fd, size);
    close(fd);
    if (!buf->pool) {
        talloc_free(buf);
        return NULL;
    }

    buf->buffer = wl_shm_pool_create_buffer(buf->pool, 0, width, height, stride,
                                            WL_SHM_FORMAT_ARGB8888);
    if (!buf->buffer) {
        talloc_free(buf);
        return NULL;
    }

    wl_buffer_add_listener(buf->buffer, &buffer_listener, buf);
    talloc_set_destructor(buf, buffer_destroy);
    return buf;
}

static void clear_free_buffers(struct vo *vo)
{
    struct priv *p = vo->priv;
    while (p->free_buffers) {
        struct buffer *buf = p->free_buffers;
        p->free_buffers = buf->next;
        talloc_free(buf);
    }
}

static bool ensure_video_placeholder(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct vo_wayland_state *wl = vo->wl;

    if (!wl || p->solid_buffer)
        return true;

    if (wl->single_pixel_manager) {
        p->solid_buffer =
            wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
                wl->single_pixel_manager, 0, 0, 0, 0);
        return p->solid_buffer != NULL;
    }

    if (!wl->shm)
        return false;

    int fd = vo_wayland_allocate_memfd(vo, 4);
    if (fd < 0)
        return false;

    uint32_t *pixel = mmap(NULL, 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixel == MAP_FAILED) {
        close(fd);
        return false;
    }
    *pixel = 0;

    p->solid_buffer_pool = wl_shm_create_pool(wl->shm, fd, 4);
    close(fd);
    munmap(pixel, 4);
    if (!p->solid_buffer_pool)
        return false;

    p->solid_buffer = wl_shm_pool_create_buffer(
        p->solid_buffer_pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    return p->solid_buffer != NULL;
}

static void map_video_surface(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct vo_wayland_state *wl = vo->wl;

    if (!wl || !wl->video_surface)
        return;

    if (!ensure_video_placeholder(vo)) {
        MP_ERR(vo, "failed to create Starfish video placeholder buffer\n");
        return;
    }

    wl_surface_attach(wl->video_surface, p->solid_buffer, 0, 0);
    wl_surface_damage_buffer(wl->video_surface, 0, 0, 1, 1);
    wl_surface_attach(wl->surface, p->solid_buffer, 0, 0);
    wl_surface_damage_buffer(wl->surface, 0, 0, 1, 1);
}

static void update_external_osd_geometry(struct vo *vo)
{
    struct priv *p = vo->priv;

    if (vo->wl || !vo->params)
        return;

    const char *width_env = getenv("STARFISH_WINDOW_WIDTH");
    const char *height_env = getenv("STARFISH_WINDOW_HEIGHT");
    int width = width_env ? atoi(width_env) : 0;
    int height = height_env ? atoi(height_env) : 0;

    if (width <= 0)
        width = vo->params->w;
    if (height <= 0)
        height = vo->params->h;

    vo->dwidth = width;
    vo->dheight = height;

    struct mp_rect src, dst;
    vo_get_src_dst_rects(vo, &src, &dst, &p->osd);
    osd_resize(vo->osd, p->osd);

    mp_mutex_lock(&vo->params_mutex);
    p->target_params.w = mp_rect_w(dst);
    p->target_params.h = mp_rect_h(dst);
    p->target_params.rotate = vo->params ? (vo->params->rotate % 90) * 90 : 0;
    p->target_params.vflip = vo->params && vo->params->vflip;
    vo->target_params = &p->target_params;
    mp_mutex_unlock(&vo->params_mutex);

    if (!p->logged_resize) {
        MP_INFO(vo,
                "Starfish external resize: window=%dx%d src=%d,%d %dx%d dst=%d,%d %dx%d osd=%dx%d margins=%d,%d,%d,%d\n",
                width, height, src.x0, src.y0, mp_rect_w(src), mp_rect_h(src),
                dst.x0, dst.y0, mp_rect_w(dst), mp_rect_h(dst),
                p->osd.w, p->osd.h, p->osd.mt, p->osd.mb, p->osd.ml, p->osd.mr);
        p->logged_resize = true;
    }
}

static void render_osd_surface(struct vo *vo, double pts)
{
    struct priv *p = vo->priv;
    struct vo_wayland_state *wl = vo->wl;
    starfish_overlay_present_cb cb = NULL;
    void *cb_opaque = NULL;

    pthread_mutex_lock(&overlay_cb_lock);
    cb = overlay_present_cb;
    cb_opaque = overlay_present_opaque;
    pthread_mutex_unlock(&overlay_cb_lock);

    if (vo->dwidth <= 0 || vo->dheight <= 0) {
        if (!p->logged_osd_skip) {
            MP_INFO(vo,
                    "Starfish OSD skipped: wl=%d shm=%d osd_surface=%d cb=%d dwidth=%d dheight=%d\n",
                    !!wl, !!(wl && wl->shm), !!(wl && wl->osd_surface), !!cb,
                    vo->dwidth, vo->dheight);
            p->logged_osd_skip = true;
        }
        return;
    }

    if ((!wl || !wl->shm || !wl->osd_surface) && cb) {
        size_t size = (size_t)vo->dheight * MP_ALIGN_UP(vo->dwidth * 4, MP_IMAGE_BYTE_ALIGN);
        if (size != p->callback_size) {
            free(p->callback_pixels);
            p->callback_pixels = malloc(size);
            p->callback_size = p->callback_pixels ? size : 0;
            p->callback_stride = p->callback_pixels ? MP_ALIGN_UP(vo->dwidth * 4, MP_IMAGE_BYTE_ALIGN) : 0;
        }
        if (!p->callback_pixels) {
            MP_ERR(vo, "failed to allocate Starfish callback OSD buffer\n");
            return;
        }

        struct mp_image mpi = {0};
        mp_image_setfmt(&mpi, IMGFMT_BGRA);
        mp_image_set_size(&mpi, vo->dwidth, vo->dheight);
        mpi.params.repr.alpha = PL_ALPHA_PREMULTIPLIED;
        mpi.planes[0] = p->callback_pixels;
        mpi.stride[0] = p->callback_stride;

        memset(mpi.planes[0], 0, p->callback_size);
        osd_draw_on_image(vo->osd, p->osd, pts, 0, &mpi);
        if (!p->logged_osd_pixels) {
            bool has_pixels = false;
            for (size_t i = 0; i + 3 < p->callback_size; i += 4) {
                if (p->callback_pixels[i + 3]) {
                    has_pixels = true;
                    break;
                }
            }
            MP_INFO(vo, "Starfish OSD callback alpha=%s size=%dx%d pts=%.3f\n",
                    has_pixels ? "nonzero" : "zero", mpi.w, mpi.h, pts);
            p->logged_osd_pixels = true;
        }
        cb(cb_opaque, p->callback_pixels, mpi.w, mpi.h, mpi.stride[0]);
        return;
    }

    if (!wl || !wl->shm || !wl->osd_surface) {
        if (!p->logged_osd_skip) {
            MP_INFO(vo,
                    "Starfish OSD skipped: wl=%d shm=%d osd_surface=%d cb=%d dwidth=%d dheight=%d\n",
                    !!wl, !!(wl && wl->shm), !!(wl && wl->osd_surface), !!cb,
                    vo->dwidth, vo->dheight);
            p->logged_osd_skip = true;
        }
        return;
    }

    struct buffer *buf = p->free_buffers;
    if (buf) {
        p->free_buffers = buf->next;
    } else {
        buf = buffer_create(vo, vo->dwidth, vo->dheight);
        if (!buf) {
            MP_ERR(vo, "failed to allocate Starfish OSD buffer: %s\n", strerror(errno));
            return;
        }
    }

    memset(buf->mpi.planes[0], 0, buf->size);
    osd_draw_on_image(vo->osd, p->osd, pts, 0, &buf->mpi);
    if (!p->logged_osd_pixels) {
        uint8_t *data = buf->mpi.planes[0];
        bool has_pixels = false;
        for (size_t i = 0; i + 3 < buf->size; i += 4) {
            if (data[i + 3]) {
                has_pixels = true;
                break;
            }
        }
        MP_INFO(vo, "Starfish OSD buffer alpha=%s size=%dx%d pts=%.3f\n",
                has_pixels ? "nonzero" : "zero", buf->mpi.w, buf->mpi.h, pts);
        p->logged_osd_pixels = true;
    }

    wl_surface_attach(wl->osd_surface, buf->buffer, 0, 0);
    wl_surface_damage_buffer(wl->osd_surface, 0, 0, vo->dwidth, vo->dheight);
}

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
    vo_wayland_handle_scale(wl);
    struct mp_rect src, dst;
    vo_get_src_dst_rects(vo, &src, &dst, &p->osd);
    if (wl->video_viewport)
        wp_viewport_set_destination(wl->video_viewport,
                                    lround(mp_rect_w(dst) / wl->scaling_factor),
                                    lround(mp_rect_h(dst) / wl->scaling_factor));
    if (wl->video_subsurface)
        wl_subsurface_set_position(wl->video_subsurface,
                                   lround(dst.x0 / wl->scaling_factor),
                                   lround(dst.y0 / wl->scaling_factor));
    if (wl->osd_viewport)
        wp_viewport_set_destination(wl->osd_viewport,
                                    lround(vo->dwidth / wl->scaling_factor),
                                    lround(vo->dheight / wl->scaling_factor));
    if (wl->osd_subsurface)
        wl_subsurface_set_position(wl->osd_subsurface,
                                   lround((0 - dst.x0) / wl->scaling_factor),
                                   lround((0 - dst.y0) / wl->scaling_factor));
    mp_mutex_lock(&vo->params_mutex);
    p->target_params.w = mp_rect_w(dst);
    p->target_params.h = mp_rect_h(dst);
    p->target_params.rotate = vo->params ? (vo->params->rotate % 90) * 90 : 0;
    p->target_params.vflip = vo->params && vo->params->vflip;
    vo->target_params = &p->target_params;
    mp_mutex_unlock(&vo->params_mutex);
    set_exported_crop(vo);

    if (!p->exported_path && vo->params) {
        starfish_ctx_set_display_window(p->ctx, 0, 0, vo->params->w, vo->params->h,
                                        0, 0, width, height);
    }

    clear_free_buffers(vo);

    if (!p->logged_resize) {
        MP_INFO(vo, "Starfish resize: window=%dx%d src=%d,%d %dx%d dst=%d,%d %dx%d\n",
                width, height, src.x0, src.y0, mp_rect_w(src), mp_rect_h(src),
                dst.x0, dst.y0, mp_rect_w(dst), mp_rect_h(dst));
        p->logged_resize = true;
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
    if (vo->wl && vo->wl->osd_surface) {
        wl_surface_commit(vo->wl->osd_surface);
        wl_surface_commit(vo->wl->video_surface);
        wl_surface_commit(vo->wl->surface);
    }
    if (vo->wl && vo->wl->use_present)
        present_sync_swap(vo->wl->present);
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;

    if (!p->logged_draw_frame) {
        MP_INFO(vo, "Starfish draw_frame: current=%d redraw=%d repeat=%d dsize=%dx%d\n",
                !!frame->current, frame->redraw, frame->repeat,
                vo->dwidth, vo->dheight);
        p->logged_draw_frame = true;
    }

    mp_image_unrefp(&p->next_image);
    if (frame->current && !frame->redraw && !frame->repeat)
        p->next_image = mp_image_new_ref(frame->current);
    update_external_osd_geometry(vo);
    set_exported_crop(vo);
    map_video_surface(vo);
    render_osd_surface(vo, frame->current ? frame->current->pts : 0);
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
        return starfish_ctx_flush(p->ctx, MP_NOPTS_VALUE) ? VO_TRUE : VO_ERROR;
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
    p->target_params = *params;
    mp_image_params_restore_dovi_mapping(&p->target_params);
    mp_image_params_guess_csp(&p->target_params);
    mp_mutex_lock(&vo->params_mutex);
    vo->target_params = &p->target_params;
    mp_mutex_unlock(&vo->params_mutex);
    starfish_ctx_set_video_geometry(p->ctx, params->w, params->h, 0);
    if (!vo->wl) {
        update_external_osd_geometry(vo);
        return 0;
    }
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
    clear_free_buffers(vo);
    if (p->solid_buffer)
        wl_buffer_destroy(p->solid_buffer);
    if (p->solid_buffer_pool)
        wl_shm_pool_destroy(p->solid_buffer_pool);
    free(p->callback_pixels);
    talloc_free(p->window_id);
    if (p->exported)
        wl_webos_exported_destroy(p->exported);
    starfish_ctx_set_current(NULL);
    if (vo->hwdec_devs) {
        hwdec_devices_remove(vo->hwdec_devs, &p->hwctx);
        hwdec_devices_destroy(vo->hwdec_devs);
        vo->hwdec_devs = NULL;
    }
    mp_mutex_lock(&vo->params_mutex);
    vo->target_params = NULL;
    mp_mutex_unlock(&vo->params_mutex);
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
