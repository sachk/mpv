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

#ifndef MP_SUB_RECOLOR_H
#define MP_SUB_RECOLOR_H

#include <stdbool.h>
#include <stdint.h>

enum mp_sub_recolor_mode {
    MP_SUB_RECOLOR_REPLACE = 0,
    MP_SUB_RECOLOR_RETINT = 1,
};

struct mp_sub_recolor {
    uint32_t color;         // 0xAARRGGBB target fill; a == 0 disables recoloring
    uint32_t outline_color; // 0xAARRGGBB; a == 0 keeps the authored outline
    int mode;               // enum mp_sub_recolor_mode
    int ink_threshold;      // alpha at or below this does not count as ink
};

// Recolor a straight-alpha 0xAARRGGBB palette in place.
//
// counts[i] must hold how many pixels of the bitmap use palette index i; the
// glyph fill is the most used opaque entry, which is far more reliable than
// picking by saturation (karaoke colors win) or by alpha (second-speaker text
// wins). Alpha is never modified, and entries that are not part of the
// outline-to-fill antialiasing ramp are left alone.
//
// Returns true if any entry changed.
bool mp_sub_recolor_palette(uint32_t *pal, int n_colors, const int *counts,
                            const struct mp_sub_recolor *opts);

#endif
