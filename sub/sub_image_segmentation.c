/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "common/common.h"
#include "sub/osd.h"
#include "sub_image_segmentation.h"

static unsigned int alpha_at(const uint8_t *pixel)
{
    uint32_t value;
    memcpy(&value, pixel, sizeof(value));
    return value >> 24;
}

int mp_image_subtitle_split_columns(const struct sub_bitmap *source, int extend, struct sub_bitmap *output,
    int output_size, int *occupied, int occupied_size)
{
    if (!source || !output || output_size < 1)
        return 0;

    int ink_width = source->w - extend * 2;
    int ink_height = source->h - extend * 2;
    if (!source->bitmap || ink_width <= 0 || ink_height <= 0 || !occupied || occupied_size < ink_width) {
        output[0] = *source;
        return 1;
    }

    memset(occupied, 0, ink_width * sizeof(occupied[0]));
    for (int y = extend; y < source->h - extend; y++) {
        const uint8_t *row = (const uint8_t *)source->bitmap + y * source->stride;
        for (int x = 0; x < ink_width; x++)
            occupied[x] |= alpha_at(row + (x + extend) * 4) > 8;
    }

    int split_gap = MPMAX(extend * 2 + 1, ink_height / 2);
    int groups = 0;
    int previous = -split_gap - 1;
    for (int x = 0; x < ink_width; x++) {
        if (!occupied[x])
            continue;
        if (x - previous > split_gap)
            groups++;
        previous = x;
    }
    if (groups < 2 || groups > output_size) {
        output[0] = *source;
        return 1;
    }

    int out_count = 0;
    int group_start = -1;
    previous = -1;
    for (int x = 0; x <= ink_width; x++) {
        bool ink = x < ink_width && occupied[x];
        bool boundary = ink && group_start >= 0 && x - previous > split_gap;
        if (boundary || (x == ink_width && group_start >= 0)) {
            int view_x0 = group_start;
            int view_x1 = MPMIN(source->w, previous + 1 + extend * 2);
            int display_x0 = lrint(view_x0 * source->dw / (double)source->w);
            int display_x1 = lrint(view_x1 * source->dw / (double)source->w);
            struct sub_bitmap part = *source;
            part.bitmap = (uint8_t *)part.bitmap + view_x0 * 4;
            part.src_x += view_x0;
            part.x += display_x0;
            part.w = view_x1 - view_x0;
            part.dw = display_x1 - display_x0;
            output[out_count++] = part;
            group_start = -1;
        }
        if (ink) {
            if (group_start < 0)
                group_start = x;
            previous = x;
        }
    }

    return out_count;
}
