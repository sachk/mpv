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

#include <math.h>

#include "sub_recolor.h"

// Palette entries are classified in Oklab because "is this the outline or the
// fill" is a lightness question, and sRGB luma answers it wrong for saturated
// colors: a fully saturated blue and a mid grey have similar luma but very
// different perceived lightness.
//
// The antialiasing ramp is recovered in *gamma-encoded* sRGB instead. What the
// ramp encodes is glyph coverage misfiled as color, and it was authored in a
// gamma-encoded space (effectively YCbCr, which is an affine transform of
// gamma-encoded RGB, so straight lines stay straight). Recovering coverage in
// linear light would change apparent stroke weight, and the text would visibly
// fatten or thin.

// Chroma below which an entry can serve as the outline. Black and near-black
// greys qualify; a dark blue sign does not.
#define OUTLINE_MAX_CHROMA 0.10f
// Minimum Oklab lightness between outline and fill for the pair to be usable.
#define MIN_RAMP_LIGHTNESS 0.10f
// Distance from the outline-to-fill line, in gamma sRGB units, within which an
// entry counts as part of the ramp rather than as a deliberate second color.
#define RAMP_EPSILON 0.06f

struct color3 {
    float v[3];
};

static float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float linear_to_srgb(float c)
{
    if (c <= 0.0f)
        return 0.0f;
    if (c >= 1.0f)
        return 1.0f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static struct color3 srgb_to_oklab(struct color3 c)
{
    float r = srgb_to_linear(c.v[0]);
    float g = srgb_to_linear(c.v[1]);
    float b = srgb_to_linear(c.v[2]);

    float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);

    return (struct color3){{
        0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s,
    }};
}

static struct color3 oklab_to_srgb(struct color3 c)
{
    float l = c.v[0] + 0.3963377774f * c.v[1] + 0.2158037573f * c.v[2];
    float m = c.v[0] - 0.1055613458f * c.v[1] - 0.0638541728f * c.v[2];
    float s = c.v[0] - 0.0894841775f * c.v[1] - 1.2914855480f * c.v[2];

    l = l * l * l;
    m = m * m * m;
    s = s * s * s;

    return (struct color3){{
        linear_to_srgb(4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s),
        linear_to_srgb(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s),
        linear_to_srgb(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s),
    }};
}

static struct color3 unpack_srgb(uint32_t argb)
{
    return (struct color3){{
        ((argb >> 16) & 0xFF) / 255.0f,
        ((argb >> 8) & 0xFF) / 255.0f,
        (argb & 0xFF) / 255.0f,
    }};
}

static uint32_t pack_srgb(struct color3 c, uint8_t alpha)
{
    uint32_t out = (uint32_t)alpha << 24;
    for (int n = 0; n < 3; n++) {
        float v = c.v[n] < 0.0f ? 0.0f : (c.v[n] > 1.0f ? 1.0f : c.v[n]);
        out |= (uint32_t)lrintf(v * 255.0f) << (16 - n * 8);
    }
    return out;
}

static float chroma_of(struct color3 lab)
{
    return hypotf(lab.v[1], lab.v[2]);
}

bool mp_sub_recolor_palette(uint32_t *pal, int n_colors, const int *counts,
                            const struct mp_sub_recolor *opts)
{
    if (n_colors <= 0 || !(opts->color >> 24))
        return false;

    // Find the outline: the darkest near-achromatic opaque entry. A missing
    // outline is normal (some VOBSUBs have none), and black stands in for it.
    int outline_idx = -1;
    float outline_l = 0.0f;
    for (int n = 0; n < n_colors; n++) {
        if ((int)(pal[n] >> 24) <= opts->ink_threshold)
            continue;
        struct color3 lab = srgb_to_oklab(unpack_srgb(pal[n]));
        if (chroma_of(lab) > OUTLINE_MAX_CHROMA)
            continue;
        if (outline_idx < 0 || lab.v[0] < outline_l) {
            outline_idx = n;
            outline_l = lab.v[0];
        }
    }

    struct color3 outline_srgb = outline_idx < 0 ? (struct color3){{0.0f, 0.0f, 0.0f}}
                                                 : unpack_srgb(pal[outline_idx]);
    struct color3 outline_lab = srgb_to_oklab(outline_srgb);

    // Find the fill: the most used opaque entry that is not the outline.
    int fill_idx = -1;
    for (int n = 0; n < n_colors; n++) {
        if (n == outline_idx || (int)(pal[n] >> 24) <= opts->ink_threshold)
            continue;
        if (fill_idx < 0 || counts[n] > counts[fill_idx])
            fill_idx = n;
    }
    if (fill_idx < 0 || counts[fill_idx] <= 0)
        return false;

    struct color3 fill_srgb = unpack_srgb(pal[fill_idx]);
    struct color3 fill_lab = srgb_to_oklab(fill_srgb);
    if (fill_lab.v[0] - outline_lab.v[0] < MIN_RAMP_LIGHTNESS)
        return false;

    // The ramp direction, in the space the ramp was authored in.
    struct color3 dir;
    float dir_len2 = 0.0f;
    for (int n = 0; n < 3; n++) {
        dir.v[n] = fill_srgb.v[n] - outline_srgb.v[n];
        dir_len2 += dir.v[n] * dir.v[n];
    }
    if (dir_len2 < 1e-6f)
        return false;

    struct color3 target_lab = srgb_to_oklab(unpack_srgb(opts->color));
    struct color3 target_outline_lab = (opts->outline_color >> 24)
        ? srgb_to_oklab(unpack_srgb(opts->outline_color)) : outline_lab;

    bool changed = false;
    for (int n = 0; n < n_colors; n++) {
        uint8_t alpha = pal[n] >> 24;
        if ((int)alpha <= opts->ink_threshold)
            continue;

        struct color3 srgb = unpack_srgb(pal[n]);

        // Project onto the outline-to-fill line; t is the recovered coverage.
        float dot = 0.0f;
        for (int c = 0; c < 3; c++)
            dot += (srgb.v[c] - outline_srgb.v[c]) * dir.v[c];
        float t = dot / dir_len2;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);

        // Entries away from the line are a different color on purpose: a
        // translated sign, a second speaker, karaoke. Leave them alone.
        float residual = 0.0f;
        for (int c = 0; c < 3; c++) {
            float d = srgb.v[c] - (outline_srgb.v[c] + t * dir.v[c]);
            residual += d * d;
        }
        if (residual > RAMP_EPSILON * RAMP_EPSILON)
            continue;

        struct color3 out;
        if (opts->mode == MP_SUB_RECOLOR_RETINT) {
            // Keep this entry's own lightness, take hue and chroma from the
            // target, faded out toward the outline. Gradient fills survive.
            struct color3 lab = srgb_to_oklab(srgb);
            out = (struct color3){{
                lab.v[0],
                target_lab.v[1] * t,
                target_lab.v[2] * t,
            }};
        } else {
            for (int c = 0; c < 3; c++) {
                out.v[c] = target_outline_lab.v[c]
                    + t * (target_lab.v[c] - target_outline_lab.v[c]);
            }
        }

        uint32_t packed = pack_srgb(oklab_to_srgb(out), alpha);
        changed |= packed != pal[n];
        pal[n] = packed;
    }

    return changed;
}
