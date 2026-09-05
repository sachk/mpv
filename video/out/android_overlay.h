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

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Subtitles and OSD for a video output that has no surface of its own to draw
// them on. MediaCodec owns the video plane and writes decoded frames straight
// to it; nothing else may touch it. So the OSD is rasterised on its own and
// handed to the embedder, which already has a layer above the video -- the
// same arrangement, and the same two calls, that the Starfish output uses on
// webOS.
//
// acquire is asked for somewhere to draw a width x height premultiplied BGRA
// image that belongs at (x, y) in the window, and returns the first byte of it
// along with the row stride and an opaque handle. present hands that handle
// back once the drawing is done, or with visible = false when there is nothing
// to show and whatever is on screen should go away.

#if defined(__GNUC__)
#define ANDROID_OVERLAY_API __attribute__((visibility("default")))
#else
#define ANDROID_OVERLAY_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t *(*android_overlay_acquire_cb)(void *opaque, int x, int y,
                                               int width, int height,
                                               int *stride, void **buffer);
typedef void (*android_overlay_present_cb)(void *opaque, void *buffer,
                                           bool visible);

ANDROID_OVERLAY_API void android_overlay_set_callbacks(
    android_overlay_acquire_cb acquire_cb,
    android_overlay_present_cb present_cb, void *opaque);

#ifdef __cplusplus
}
#endif
