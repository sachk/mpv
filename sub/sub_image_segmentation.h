/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef MP_SUB_IMAGE_SEGMENTATION_H
#define MP_SUB_IMAGE_SEGMENTATION_H

struct sub_bitmap;

// Split one decoded BGRA subtitle rectangle at screen-sized transparent
// column gaps. Each returned view remains backed by the source bitmap.
int mp_image_subtitle_split_columns(const struct sub_bitmap *source, int extend, struct sub_bitmap *output,
    int output_size, int *occupied, int occupied_size);

#endif
