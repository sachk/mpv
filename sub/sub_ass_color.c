/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "sub_ass_color.h"

static bool same_rgb(uint32_t left, uint32_t right)
{
    return (left & 0xFFFFFF00u) == (right & 0xFFFFFF00u);
}

uint32_t mp_ass_text_color_override(uint32_t color, uint32_t desired, const ASS_Style *styles, int num_styles)
{
    bool text = false;
    bool decoration = false;
    for (int n = 0; n < num_styles; n++) {
        const ASS_Style *style = &styles[n];
        text |= same_rgb(color, style->PrimaryColour) || same_rgb(color, style->SecondaryColour);
        decoration |= same_rgb(color, style->OutlineColour) || same_rgb(color, style->BackColour);
    }

    // Unknown colours come from inline overrides and are text more often than
    // decoration. A known primary colour wins if a script reused it as an
    // outline colour too.
    if (text || !decoration)
        return (desired & 0xFFFFFF00u) | (color & 0xFFu);
    return color;
}
