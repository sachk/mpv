#include "starfish_ctx.h"

#include <atomic>
#include <mutex>

#include "common/msg.h"
#include "mpv_talloc.h"
#include "video/hwdec.h"

struct starfish_ctx {
    std::atomic<int> refs;
    struct mp_log *log;
};

static std::mutex g_current_lock;
static struct starfish_ctx *g_current_ctx;

struct starfish_ctx *starfish_ctx_create(struct mp_log *log)
{
    struct starfish_ctx *ctx = new starfish_ctx();
    ctx->refs.store(1, std::memory_order_relaxed);
    ctx->log = mp_log_new(NULL, log, "starfish");
    return ctx;
}

struct starfish_ctx *starfish_ctx_retain(struct starfish_ctx *ctx)
{
    if (ctx)
        ctx->refs.fetch_add(1, std::memory_order_relaxed);
    return ctx;
}

void starfish_ctx_unref(struct starfish_ctx *ctx)
{
    if (!ctx)
        return;
    if (ctx->refs.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;
    talloc_free(ctx->log);
    delete ctx;
}

struct starfish_ctx *starfish_ctx_from_hwdec(struct mp_hwdec_ctx *hwctx)
{
    return hwctx ? (struct starfish_ctx *)hwctx->conversion_config : NULL;
}

bool starfish_ctx_set_current(struct starfish_ctx *ctx)
{
    std::lock_guard<std::mutex> lock(g_current_lock);
    if (ctx == g_current_ctx)
        return true;
    starfish_ctx_retain(ctx);
    starfish_ctx_unref(g_current_ctx);
    g_current_ctx = ctx;
    return true;
}

struct starfish_ctx *starfish_ctx_get_current(void)
{
    std::lock_guard<std::mutex> lock(g_current_lock);
    return starfish_ctx_retain(g_current_ctx);
}
