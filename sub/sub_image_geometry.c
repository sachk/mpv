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

#include "common/common.h"
#include "sub/osd.h"
#include "sub_image_geometry.h"

struct mp_rect mp_image_subtitle_viewport(struct mp_osd_res output, bool allow_margins)
{
    struct mp_rect full = { 0, 0, output.w, output.h };
    if (allow_margins)
        return full;

    struct mp_rect video = {
        .x0 = MPMAX(0, output.ml),
        .y0 = MPMAX(0, output.mt),
        .x1 = MPMIN(output.w, output.w - output.mr),
        .y1 = MPMIN(output.h, output.h - output.mb),
    };
    return mp_rect_w(video) > 0 && mp_rect_h(video) > 0 ? video : full;
}
static struct mp_rect part_ink(const struct sub_bitmap *part, int extend)
{
    int pad_x = part->w > 0 ? lrint(extend * part->dw / (double)part->w) : 0;
    int pad_y = part->h > 0 ? lrint(extend * part->dh / (double)part->h) : 0;
    return (struct mp_rect) {
        part->x + pad_x,
        part->y + pad_y,
        part->x + part->dw - pad_x,
        part->y + part->dh - pad_y,
    };
}

static bool overlaps_vertically(struct mp_rect part, int y0, int y1)
{
    return part.y0 < y1 && part.y1 > y0;
}

// Pull distant objects on each authored line together. Parts that overlap or
// already have ordinary word spacing retain their exact relative positions;
// only screen-sized gaps between independent objects are reduced. Processing
// rows independently preserves authored multiline layouts.
static void compact_distant_groups(struct sub_bitmaps *imgs, int extend)
{
    for (int leader = 0; leader < imgs->num_parts; leader++) {
        struct mp_rect seed = part_ink(&imgs->parts[leader], extend);
        int row_y0 = seed.y0;
        int row_y1 = seed.y1;

        bool changed;
        do {
            changed = false;
            for (int i = 0; i < imgs->num_parts; i++) {
                struct mp_rect ink = part_ink(&imgs->parts[i], extend);
                if (!overlaps_vertically(ink, row_y0, row_y1))
                    continue;
                int y0 = MPMIN(row_y0, ink.y0);
                int y1 = MPMAX(row_y1, ink.y1);
                changed |= y0 != row_y0 || y1 != row_y1;
                row_y0 = y0;
                row_y1 = y1;
            }
        } while (changed);

        bool handled = false;
        for (int i = 0; i < leader; i++) {
            if (overlaps_vertically(part_ink(&imgs->parts[i], extend), row_y0, row_y1)) {
                handled = true;
                break;
            }
        }
        if (handled)
            continue;

        int original_x0 = INT_MAX;
        int original_x1 = INT_MIN;
        for (int i = 0; i < imgs->num_parts; i++) {
            struct mp_rect ink = part_ink(&imgs->parts[i], extend);
            if (!overlaps_vertically(ink, row_y0, row_y1))
                continue;
            original_x0 = MPMIN(original_x0, ink.x0);
            original_x1 = MPMAX(original_x1, ink.x1);
        }
        if (original_x0 >= original_x1)
            continue;

        int row_height = MPMAX(1, row_y1 - row_y0);
        int desired_gap = MPMAX(1, row_height / 2);
        int distant_gap = MPMAX(extend * 2 + 1, row_height * 3 / 2);

        int cursor = original_x0;
        while (cursor < original_x1) {
            int covered_to = cursor;
            bool expanded;
            do {
                expanded = false;
                for (int i = 0; i < imgs->num_parts; i++) {
                    struct mp_rect ink = part_ink(&imgs->parts[i], extend);
                    if (!overlaps_vertically(ink, row_y0, row_y1) || ink.x0 > covered_to || ink.x1 <= cursor)
                        continue;
                    if (ink.x1 > covered_to) {
                        covered_to = ink.x1;
                        expanded = true;
                    }
                }
            } while (expanded);

            int next = INT_MAX;
            for (int i = 0; i < imgs->num_parts; i++) {
                struct mp_rect ink = part_ink(&imgs->parts[i], extend);
                if (overlaps_vertically(ink, row_y0, row_y1) && ink.x0 > covered_to)
                    next = MPMIN(next, ink.x0);
            }
            if (next == INT_MAX)
                break;

            int gap = next - covered_to;
            if (gap > distant_gap) {
                int dx = gap - desired_gap;
                for (int i = 0; i < imgs->num_parts; i++) {
                    struct mp_rect ink = part_ink(&imgs->parts[i], extend);
                    if (overlaps_vertically(ink, row_y0, row_y1) && ink.x0 >= next)
                        imgs->parts[i].x -= dx;
                }
                next -= dx;
            }
            cursor = next;
        }

        int compact_x0 = INT_MAX;
        int compact_x1 = INT_MIN;
        for (int i = 0; i < imgs->num_parts; i++) {
            struct mp_rect ink = part_ink(&imgs->parts[i], extend);
            if (!overlaps_vertically(ink, row_y0, row_y1))
                continue;
            compact_x0 = MPMIN(compact_x0, ink.x0);
            compact_x1 = MPMAX(compact_x1, ink.x1);
        }
        int center_dx = (original_x0 + original_x1 - compact_x0 - compact_x1) / 2;
        for (int i = 0; i < imgs->num_parts; i++) {
            if (overlaps_vertically(part_ink(&imgs->parts[i], extend), row_y0, row_y1))
                imgs->parts[i].x += center_dx;
        }
    }
}

void mp_image_subtitle_reposition_all(struct sub_bitmaps *imgs, int extend, struct mp_rect visible, float sub_pos)
{
    if (!imgs || imgs->num_parts < 1)
        return;

    compact_distant_groups(imgs, extend);

    struct mp_rect ink = { INT_MAX, INT_MAX, INT_MIN, INT_MIN };
    for (int i = 0; i < imgs->num_parts; i++) {
        struct mp_rect part = part_ink(&imgs->parts[i], extend);
        ink.x0 = MPMIN(ink.x0, part.x0);
        ink.y0 = MPMIN(ink.y0, part.y0);
        ink.x1 = MPMAX(ink.x1, part.x1);
        ink.y1 = MPMAX(ink.y1, part.y1);
    }
    if (ink.x0 >= ink.x1 || ink.y0 >= ink.y1)
        return;

    int dx = (visible.x0 + visible.x1 - ink.x0 - ink.x1) / 2;
    int target = visible.y1 - lrint(mp_rect_h(visible) * (100.0f - sub_pos) / 100.0f);
    int dy = MPMAX(target - ink.y1, visible.y0 - ink.y0);
    for (int i = 0; i < imgs->num_parts; i++) {
        imgs->parts[i].x += dx;
        imgs->parts[i].y += dy;
    }
}

static bool bitmap_row_has_ink(const struct sub_bitmap *part, int y)
{
    const uint8_t *row = (const uint8_t *)part->bitmap + y * part->stride;
    for (int x = 0; x < part->w; x++) {
        if (row[x * 4 + 3] >= 16)
            return true;
    }
    return false;
}

static void add_detected_line(
    const struct sub_bitmap *part, int source_height, float target, float *height_sum, int *height_count)
{
    float display_height = source_height * part->dh / (float)part->h;
    if (display_height >= target * 0.2f && display_height <= target * 4.0f) {
        *height_sum += display_height;
        *height_count += 1;
    }
}

float mp_image_subtitle_text_scale(
    const struct sub_bitmaps *imgs, int extend, float user_scale, struct mp_rect visible)
{
    if (!imgs || imgs->num_parts < 1 || user_scale <= 0.0f || mp_rect_h(visible) <= 0)
        return user_scale;

    // mpv's default 55-point text occupies about 5.2% of a 720p-relative
    // frame after libass font metrics are applied. Bitmap subtitle canvases
    // vary wildly, so measure occupied alpha-row bands rather than their boxes.
    float target = mp_rect_h(visible) * 0.052f;
    float height_sum = 0.0f;
    int height_count = 0;
    if (imgs->format == SUBBITMAP_BGRA) {
        for (int i = 0; i < imgs->num_parts; i++) {
            const struct sub_bitmap *part = &imgs->parts[i];
            if (!part->bitmap || part->w <= 0 || part->h <= 0 || part->dh <= 0)
                continue;
            int band_start = -1;
            int last_ink = -1;
            int join_gap = MPMAX(1, part->h / 80);
            for (int y = 0; y < part->h; y++) {
                if (bitmap_row_has_ink(part, y)) {
                    if (band_start < 0)
                        band_start = y;
                    last_ink = y;
                } else if (band_start >= 0 && y - last_ink > join_gap) {
                    add_detected_line(part, last_ink - band_start + 1, target, &height_sum, &height_count);
                    band_start = -1;
                    last_ink = -1;
                }
            }
            if (band_start >= 0)
                add_detected_line(part, last_ink - band_start + 1, target, &height_sum, &height_count);
        }
    }

    if (height_count == 0) {
        for (int i = 0; i < imgs->num_parts; i++) {
            struct mp_rect ink = part_ink(&imgs->parts[i], extend);
            int height = ink.y1 - ink.y0;
            if (height > 0) {
                height_sum += height;
                height_count++;
            }
        }
    }
    if (height_count == 0)
        return user_scale;

    float detected = height_sum / height_count;
    return MPCLAMP(target * user_scale / detected, 0.5f, 2.5f);
}

void mp_image_subtitle_scale_all(struct sub_bitmaps *imgs, int extend, float scale, struct mp_rect visible)
{
    if (!imgs || imgs->num_parts < 1 || scale == 1.0f || scale <= 0.0f)
        return;

    float ink_x0 = INFINITY;
    float ink_y0 = INFINITY;
    float ink_x1 = -INFINITY;
    float ink_y1 = -INFINITY;
    for (int i = 0; i < imgs->num_parts; i++) {
        struct sub_bitmap *part = &imgs->parts[i];
        if (part->w <= 0 || part->h <= 0 || part->dw <= 0 || part->dh <= 0)
            continue;
        float pad_x = extend * part->dw / (float)part->w;
        float pad_y = extend * part->dh / (float)part->h;
        ink_x0 = fminf(ink_x0, part->x + pad_x);
        ink_y0 = fminf(ink_y0, part->y + pad_y);
        ink_x1 = fmaxf(ink_x1, part->x + part->dw - pad_x);
        ink_y1 = fmaxf(ink_y1, part->y + part->dh - pad_y);
    }
    if (!isfinite(ink_x0) || ink_x0 >= ink_x1 || ink_y0 >= ink_y1)
        return;

    float origin_x = (ink_x0 + ink_x1) / 2.0f;
    float origin_y = ink_y1;
    float scaled_ink_x0 = origin_x + (ink_x0 - origin_x) * scale;
    float scaled_ink_x1 = origin_x + (ink_x1 - origin_x) * scale;
    float scaled_ink_y0 = origin_y + (ink_y0 - origin_y) * scale;
    float scaled_ink_y1 = origin_y;
    float dx = scaled_ink_x0 < visible.x0 ? visible.x0 - scaled_ink_x0 : 0.0f;
    if (scaled_ink_x1 + dx > visible.x1)
        dx = visible.x1 - scaled_ink_x1;
    if (scaled_ink_x1 - scaled_ink_x0 > mp_rect_w(visible))
        dx = visible.x0 - scaled_ink_x0;
    float dy = scaled_ink_y0 < visible.y0 ? visible.y0 - scaled_ink_y0 : 0.0f;
    if (scaled_ink_y1 - scaled_ink_y0 > mp_rect_h(visible))
        dy = visible.y0 - scaled_ink_y0;

    for (int i = 0; i < imgs->num_parts; i++) {
        struct sub_bitmap *part = &imgs->parts[i];
        float x0 = origin_x + (part->x - origin_x) * scale + dx;
        float y0 = origin_y + (part->y - origin_y) * scale + dy;
        float x1 = origin_x + (part->x + part->dw - origin_x) * scale + dx;
        float y1 = origin_y + (part->y + part->dh - origin_y) * scale + dy;
        int rx0 = lrintf(x0);
        int ry0 = lrintf(y0);
        int rx1 = lrintf(x1);
        int ry1 = lrintf(y1);
        part->x = rx0;
        part->y = ry0;
        part->dw = MPMAX(1, rx1 - rx0);
        part->dh = MPMAX(1, ry1 - ry0);
    }
}
