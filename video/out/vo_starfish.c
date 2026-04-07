/*
 * This file is part of mpv.
 */

#include "video/hwdec.h"
#include "video/mp_image.h"
#include "video/out/vo.h"
#include "video/out/starfish/starfish_ctx.h"

struct priv {
    struct mp_image *next_image;
    struct mp_hwdec_ctx hwctx;
    struct starfish_ctx *ctx;
};

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;

    p->ctx = starfish_ctx_create(vo->log);
    if (!p->ctx)
        return -1;

    vo->hwdec_devs = hwdec_devices_create();
    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = "starfish",
        .hw_imgfmt = IMGFMT_STARFISH,
        .conversion_config = p->ctx,
    };
    hwdec_devices_add(vo->hwdec_devs, &p->hwctx);
    starfish_ctx_set_current(p->ctx);
    return 0;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    mp_image_unrefp(&p->next_image);
}

static bool draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;

    mp_image_unrefp(&p->next_image);
    if (!frame->redraw && !frame->repeat)
        p->next_image = mp_image_new_ref(frame->current);
    return VO_TRUE;
}

static int query_format(struct vo *vo, int format)
{
    return format == IMGFMT_STARFISH;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    return VO_NOTIMPL;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;

    mp_image_unrefp(&p->next_image);
    starfish_ctx_set_current(NULL);
    if (vo->hwdec_devs) {
        hwdec_devices_remove(vo->hwdec_devs, &p->hwctx);
        hwdec_devices_destroy(vo->hwdec_devs);
        vo->hwdec_devs = NULL;
    }
    starfish_ctx_unref(p->ctx);
}

const struct vo_driver video_out_starfish = {
    .description = "LG webOS Starfish",
    .name = "starfish",
    .caps = VO_CAP_NORETAIN,
    .preinit = preinit,
    .query_format = query_format,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .reconfig = reconfig,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
};
