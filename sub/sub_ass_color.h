/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef MP_SUB_ASS_COLOR_H
#define MP_SUB_ASS_COLOR_H

#include <stdbool.h>
#include <stdint.h>

#include <ass/ass.h>

uint32_t mp_ass_text_color_override(uint32_t color, uint32_t desired, const ASS_Style *styles, int num_styles);

#endif
