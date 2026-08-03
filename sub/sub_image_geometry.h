/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef MP_SUB_IMAGE_GEOMETRY_H
#define MP_SUB_IMAGE_GEOMETRY_H

#include "common/common.h"

struct sub_bitmaps;

void mp_image_subtitle_reposition_all(struct sub_bitmaps *imgs, int extend, struct mp_rect visible, float sub_pos);
void mp_image_subtitle_scale_all(struct sub_bitmaps *imgs, int extend, float scale, struct mp_rect visible);

#endif
