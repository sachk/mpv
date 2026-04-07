#pragma once

#include <stdbool.h>

struct mp_hwdec_ctx;
struct mp_log;

struct starfish_ctx;

struct starfish_ctx *starfish_ctx_create(struct mp_log *log);
struct starfish_ctx *starfish_ctx_retain(struct starfish_ctx *ctx);
void starfish_ctx_unref(struct starfish_ctx *ctx);

struct starfish_ctx *starfish_ctx_from_hwdec(struct mp_hwdec_ctx *hwctx);

bool starfish_ctx_set_current(struct starfish_ctx *ctx);
struct starfish_ctx *starfish_ctx_get_current(void);
