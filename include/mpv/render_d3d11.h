/* Copyright (C) 2026 the mpv developers
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef MPV_CLIENT_API_RENDER_D3D11_H_
#define MPV_CLIENT_API_RENDER_D3D11_H_

#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Direct3D 11 backend (fork extension)
 * ------------------------------------
 *
 * Renders into an ID3D11Texture2D the API user owns, on an ID3D11Device the
 * API user created. mpv never presents: the caller is compositing the result
 * into a window of its own, which is what the OpenGL backend's FBO is for and
 * what this replaces on a D3D11 renderer.
 *
 * The device is taken rather than created, so both sides share one device and
 * one immediate context. That is what makes the video frame usable by the
 * caller's renderer with no copy.
 *
 * Threading
 * ---------
 *
 * A D3D11 immediate context is not thread-safe and mpv does not lock it, so
 * the caller must not use the device from another thread while
 * mpv_render_context_render() is running. Calling it from the thread that
 * drives the caller's own renderer, between its own draws, is the intended
 * use -- and being the same context is also what orders the finished frame
 * before whatever the caller submits next, with no fence to arrange.
 */

/**
 * Initialisation parameters for MPV_RENDER_API_TYPE_D3D11.
 *
 * Handles arrive as void pointers rather than as COM types, so that this
 * header does not drag d3d11.h into every API user. Cast the real ones in.
 */
typedef struct mpv_d3d11_init_params {
    /**
     * ID3D11Device to render on. Required. mpv takes a reference for as long
     * as the render context lives.
     */
    void *device;
} mpv_d3d11_init_params;

/**
 * The texture to render into, for MPV_RENDER_PARAM_D3D11_TEXTURE.
 */
typedef struct mpv_d3d11_texture {
    /**
     * ID3D11Texture2D, created by the same device, with D3D11_USAGE_DEFAULT,
     * neither mipmapped nor multisampled. A format with more than 8 bits per
     * component is what makes an HDR target possible; mpv renders to whatever
     * it is given and the target colorspace is set through the usual options.
     */
    void *texture;
    /**
     * Size in pixels.
     */
    int w, h;
} mpv_d3d11_texture;

#ifdef __cplusplus
}
#endif

#endif
