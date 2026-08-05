/*
 * High-quality resampling for decoded bitmap subtitles.
 *
 * This file is part of mpv and is licensed under the GNU LGPL v2.1 or later.
 */

#include <math.h>
#include <string.h>

#include "common/common.h"
#include "mpv_talloc.h"
#include "sub_sdf_scale.h"

struct plane {
    int w, h;
    float *v;
};

struct edge_offset {
    float x, y;
};

struct mp_sdf_scaler {
    struct plane alpha;
    struct plane fill;
    struct plane rgb[3];
    struct plane sdf_alpha;
    struct plane sdf_fill;
    float color0[3];
    float color1[3];
    bool two_class;
    bool clamp_far_field;
    int padding;
};

static bool plane_alloc(void *ctx, struct plane *p, int w, int h)
{
    if (w <= 0 || h <= 0 || (size_t)w > SIZE_MAX / (size_t)h)
        return false;
    size_t count = (size_t)w * h;
    p->v = talloc_realloc(ctx, p->v, float, count);
    if (!p->v)
        return false;
    p->w = w;
    p->h = h;
    memset(p->v, 0, count * sizeof(*p->v));
    return true;
}

static inline float plane_get(const struct plane *p, int x, int y)
{
    return p->v[(size_t)y * p->w + x];
}

static inline void plane_set(struct plane *p, int x, int y, float value)
{
    p->v[(size_t)y * p->w + x] = value;
}

static float half_plane_inverse(float coverage, float nx, float ny)
{
    coverage = MPCLAMP(coverage, 0.0f, 1.0f);
    float a = MPMAX(fabsf(nx), fabsf(ny));
    float b = MPMIN(fabsf(nx), fabsf(ny));
    if (a < 1e-6f)
        return coverage - 0.5f;
    float corner = b / (2.0f * a);
    if (coverage < corner)
        return -(a + b) * 0.5f + sqrtf(2.0f * a * b * coverage);
    if (coverage > 1.0f - corner)
        return (a + b) * 0.5f - sqrtf(2.0f * a * b * (1.0f - coverage));
    return a * (coverage - 0.5f);
}

static inline float clamped_get(const struct plane *p, int x, int y)
{
    return plane_get(p, MPCLAMP(x, 0, p->w - 1), MPCLAMP(y, 0, p->h - 1));
}

static inline void relax_pixel(int w, int h, struct edge_offset *offset, float *distance2, int x, int y, int dx, int dy)
{
    int qx = x + dx;
    int qy = y + dy;
    if (qx < 0 || qy < 0 || qx >= w || qy >= h)
        return;
    size_t q = (size_t)qy * w + qx;
    size_t p = (size_t)y * w + x;
    if (!isfinite(distance2[q]))
        return;
    float ox = offset[q].x + dx;
    float oy = offset[q].y + dy;
    float candidate = ox * ox + oy * oy;
    if (candidate < distance2[p]) {
        distance2[p] = candidate;
        offset[p] = (struct edge_offset) { ox, oy };
    }
}

static bool build_sdf(void *ctx, const struct plane *alpha, struct plane *out)
{
    int w = alpha->w;
    int h = alpha->h;
    size_t count = (size_t)w * h;
    struct edge_offset *offset = talloc_zero_array(ctx, struct edge_offset, count);
    float *distance2 = talloc_array(ctx, float, count);
    if (!offset || !distance2 || !plane_alloc(ctx, out, w, h)) {
        talloc_free(offset);
        talloc_free(distance2);
        return false;
    }
    for (size_t i = 0; i < count; i++)
        distance2[i] = INFINITY;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float coverage = plane_get(alpha, x, y);
            bool inside = coverage >= 0.5f;
            bool edge = coverage > 0.0f && coverage < 1.0f;
            if (!edge) {
                edge = (clamped_get(alpha, x - 1, y) >= 0.5f) != inside
                    || (clamped_get(alpha, x + 1, y) >= 0.5f) != inside
                    || (clamped_get(alpha, x, y - 1) >= 0.5f) != inside
                    || (clamped_get(alpha, x, y + 1) >= 0.5f) != inside;
            }
            if (!edge)
                continue;

            float gx = 3.0f * (clamped_get(alpha, x + 1, y - 1) - clamped_get(alpha, x - 1, y - 1))
                + 10.0f * (clamped_get(alpha, x + 1, y) - clamped_get(alpha, x - 1, y))
                + 3.0f * (clamped_get(alpha, x + 1, y + 1) - clamped_get(alpha, x - 1, y + 1));
            float gy = 3.0f * (clamped_get(alpha, x - 1, y + 1) - clamped_get(alpha, x - 1, y - 1))
                + 10.0f * (clamped_get(alpha, x, y + 1) - clamped_get(alpha, x, y - 1))
                + 3.0f * (clamped_get(alpha, x + 1, y + 1) - clamped_get(alpha, x + 1, y - 1));
            float length = sqrtf(gx * gx + gy * gy);
            float nx = length < 1e-6f ? 0.0f : gx / length;
            float ny = length < 1e-6f ? 1.0f : gy / length;
            float distance = half_plane_inverse(coverage, nx, ny);
            size_t i = (size_t)y * w + x;
            offset[i] = (struct edge_offset) { -distance * nx, -distance * ny };
            distance2[i] = distance * distance;
        }
    }

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            relax_pixel(w, h, offset, distance2, x, y, -1, 0);
            relax_pixel(w, h, offset, distance2, x, y, 0, -1);
            relax_pixel(w, h, offset, distance2, x, y, -1, -1);
            relax_pixel(w, h, offset, distance2, x, y, 1, -1);
        }
    }
    for (int y = h - 1; y >= 0; y--) {
        for (int x = w - 1; x >= 0; x--) {
            relax_pixel(w, h, offset, distance2, x, y, 1, 0);
            relax_pixel(w, h, offset, distance2, x, y, 0, 1);
            relax_pixel(w, h, offset, distance2, x, y, 1, 1);
            relax_pixel(w, h, offset, distance2, x, y, -1, 1);
        }
    }

    for (size_t i = 0; i < count; i++) {
        float distance = isfinite(distance2[i]) ? sqrtf(distance2[i]) : 1e4f;
        out->v[i] = alpha->v[i] >= 0.5f ? distance : -distance;
    }
    talloc_free(offset);
    talloc_free(distance2);
    return true;
}

static void unbin_color(size_t index, float color[3])
{
    color[0] = ((index >> 10) & 31) / 31.0f;
    color[1] = ((index >> 5) & 31) / 31.0f;
    color[2] = (index & 31) / 31.0f;
}

struct mp_sdf_scaler *mp_sdf_scaler_create(
    void *ta_parent, const uint8_t *src, int w, int h, int stride, bool clamp_far_field)
{
    if (!src || w <= 0 || h <= 0 || stride < w * 4)
        return NULL;

    struct mp_sdf_scaler *s = talloc_zero(ta_parent, struct mp_sdf_scaler);
    if (!s)
        return NULL;
    s->padding = 4;
    s->clamp_far_field = clamp_far_field;
    int pw = w + 2 * s->padding;
    int ph = h + 2 * s->padding;
    if (!plane_alloc(s, &s->alpha, pw, ph))
        goto error;
    for (int c = 0; c < 3; c++) {
        if (!plane_alloc(s, &s->rgb[c], pw, ph))
            goto error;
    }

    size_t pixels = (size_t)w * h;
    float *straight[3] = {
        talloc_array(s, float, pixels),
        talloc_array(s, float, pixels),
        talloc_array(s, float, pixels),
    };
    uint32_t *histogram = talloc_zero_array(s, uint32_t, 1u << 15);
    if (!straight[0] || !straight[1] || !straight[2] || !histogram)
        goto error;

    float max_alpha = 0.0f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t *pixel = src + (size_t)y * stride + (size_t)x * 4;
            float a = pixel[3] / 255.0f;
            float color[3] = { pixel[2] / 255.0f, pixel[1] / 255.0f, pixel[0] / 255.0f };
            size_t i = (size_t)y * w + x;
            plane_set(&s->alpha, x + s->padding, y + s->padding, a);
            max_alpha = MPMAX(max_alpha, a);
            for (int c = 0; c < 3; c++) {
                plane_set(&s->rgb[c], x + s->padding, y + s->padding, color[c]);
                straight[c][i] = a > 1e-3f ? MPCLAMP(color[c] / a, 0.0f, 1.0f) : 0.0f;
            }
        }
    }

    float opaque_threshold = MPMAX(0.1f, max_alpha * 0.85f);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            if (plane_get(&s->alpha, x + s->padding, y + s->padding) < opaque_threshold)
                continue;
            unsigned r = lrintf(straight[0][i] * 31.0f);
            unsigned g = lrintf(straight[1][i] * 31.0f);
            unsigned b = lrintf(straight[2][i] * 31.0f);
            histogram[(r << 10) | (g << 5) | b]++;
        }
    }

    size_t mode0 = 0;
    for (size_t i = 1; i < (1u << 15); i++) {
        if (histogram[i] > histogram[mode0])
            mode0 = i;
    }
    if (histogram[mode0]) {
        unbin_color(mode0, s->color0);
        size_t mode1 = 0;
        float best = 0.0f;
        uint32_t minimum_count = MPMAX(1, histogram[mode0] / 20);
        for (size_t i = 0; i < (1u << 15); i++) {
            if (histogram[i] < minimum_count)
                continue;
            float color[3];
            unbin_color(i, color);
            float dr = color[0] - s->color0[0];
            float dg = color[1] - s->color0[1];
            float db = color[2] - s->color0[2];
            float separation = dr * dr + dg * dg + db * db;
            float score = separation * sqrtf(histogram[i]);
            if (separation > 0.02f && score > best) {
                best = score;
                mode1 = i;
            }
        }
        s->two_class = best > 0.0f;
        if (s->two_class)
            unbin_color(mode1, s->color1);
    }

    if (s->two_class) {
        if (!plane_alloc(s, &s->fill, pw, ph))
            goto error;
        float ux = s->color1[0] - s->color0[0];
        float uy = s->color1[1] - s->color0[1];
        float uz = s->color1[2] - s->color0[2];
        float inverse_length2 = 1.0f / (ux * ux + uy * uy + uz * uz);
        double residual = 0.0;
        size_t residual_count = 0;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                size_t i = (size_t)y * w + x;
                float a = plane_get(&s->alpha, x + s->padding, y + s->padding);
                if (a < 1e-3f)
                    continue;
                float t = ((straight[0][i] - s->color0[0]) * ux + (straight[1][i] - s->color0[1]) * uy
                              + (straight[2][i] - s->color0[2]) * uz)
                    * inverse_length2;
                t = MPCLAMP(t, 0.0f, 1.0f);
                plane_set(&s->fill, x + s->padding, y + s->padding, a * t);
                if (a >= opaque_threshold) {
                    float er = straight[0][i] - (s->color0[0] + t * ux);
                    float eg = straight[1][i] - (s->color0[1] + t * uy);
                    float eb = straight[2][i] - (s->color0[2] + t * uz);
                    residual += er * er + eg * eg + eb * eb;
                    residual_count++;
                }
            }
        }
        if (residual_count && residual / residual_count > 0.02) {
            s->two_class = false;
            TA_FREEP(&s->fill.v);
            s->fill = (struct plane) { 0 };
        }
    }

    if (!build_sdf(s, &s->alpha, &s->sdf_alpha))
        goto error;
    if (s->two_class && !build_sdf(s, &s->fill, &s->sdf_fill))
        goto error;

    talloc_free(histogram);
    for (int c = 0; c < 3; c++)
        talloc_free(straight[c]);
    return s;

error:
    talloc_free(s);
    return NULL;
}

static float cubic(float a, float b, float c, float d, float t)
{
    return b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
}

static float sample_catmull_rom(const struct plane *p, float x, float y, float limit)
{
    int ix = floorf(x);
    int iy = floorf(y);
    float fx = x - ix;
    float fy = y - iy;
    float column[4];
    for (int j = 0; j < 4; j++) {
        float row[4];
        for (int i = 0; i < 4; i++) {
            row[i] = clamped_get(p, ix - 1 + i, iy - 1 + j);
            if (limit > 0.0f)
                row[i] = MPCLAMP(row[i], -limit, limit);
        }
        column[j] = cubic(row[0], row[1], row[2], row[3], fx);
    }
    return cubic(column[0], column[1], column[2], column[3], fy);
}

static bool resample_area(void *ctx, const struct plane *src, struct plane *dst, int dw, int dh)
{
    struct plane tmp = { 0 };
    if (!plane_alloc(ctx, &tmp, dw, src->h) || !plane_alloc(ctx, dst, dw, dh)) {
        talloc_free(tmp.v);
        return false;
    }
    float ratio_x = (float)src->w / dw;
    for (int y = 0; y < src->h; y++) {
        for (int x = 0; x < dw; x++) {
            float x0 = x * ratio_x;
            float x1 = x0 + ratio_x;
            float sum = 0.0f;
            int last = MPMIN((int)ceilf(x1), src->w);
            for (int i = (int)x0; i < last; i++)
                sum += plane_get(src, i, y) * (MPMIN(x1, i + 1.0f) - MPMAX(x0, (float)i));
            plane_set(&tmp, x, y, sum / ratio_x);
        }
    }
    float ratio_y = (float)src->h / dh;
    for (int y = 0; y < dh; y++) {
        float y0 = y * ratio_y;
        float y1 = y0 + ratio_y;
        int last = MPMIN((int)ceilf(y1), src->h);
        for (int x = 0; x < dw; x++) {
            float sum = 0.0f;
            for (int i = (int)y0; i < last; i++)
                sum += plane_get(&tmp, x, i) * (MPMIN(y1, i + 1.0f) - MPMAX(y0, (float)i));
            plane_set(dst, x, y, sum / ratio_y);
        }
    }
    talloc_free(tmp.v);
    return true;
}

static inline uint8_t to_byte(float value)
{
    return lrintf(MPCLAMP(value, 0.0f, 1.0f) * 255.0f);
}

void mp_sdf_scaler_render(struct mp_sdf_scaler *s, uint8_t *dst, int w, int h, int stride, float softness)
{
    if (!s || !dst || w <= 0 || h <= 0 || stride < w * 4)
        return;
    float scale_x = (float)w / s->alpha.w;
    float scale_y = (float)h / s->alpha.h;
    bool downscale = scale_x < 0.98f || scale_y < 0.98f;
    struct plane alpha = { 0 };
    struct plane secondary[3] = { { 0 } };

    if (downscale) {
        if (!resample_area(s, &s->alpha, &alpha, w, h))
            return;
        int planes = s->two_class ? 1 : 3;
        for (int c = 0; c < planes; c++) {
            const struct plane *source = s->two_class ? &s->fill : &s->rgb[c];
            if (!resample_area(s, source, &secondary[c], w, h))
                goto done;
        }
    }

    float scale = 0.5f * (scale_x + scale_y);
    float threshold_scale = scale / MPMAX(softness, 1e-3f);
    float distance_limit = s->clamp_far_field ? 3.0f : 0.0f;
    for (int y = 0; y < h; y++) {
        uint8_t *row = dst + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            float a;
            float value[3];
            if (downscale) {
                a = plane_get(&alpha, x, y);
                if (s->two_class) {
                    float fill = MPCLAMP(plane_get(&secondary[0], x, y), 0.0f, a);
                    float outline = a - fill;
                    for (int c = 0; c < 3; c++)
                        value[c] = fill * s->color1[c] + outline * s->color0[c];
                } else {
                    for (int c = 0; c < 3; c++)
                        value[c] = plane_get(&secondary[c], x, y);
                }
            } else {
                float u = (x + 0.5f) / scale_x - 0.5f;
                float v = (y + 0.5f) / scale_y - 0.5f;
                a = MPCLAMP(
                    0.5f + sample_catmull_rom(&s->sdf_alpha, u, v, distance_limit) * threshold_scale, 0.0f, 1.0f);
                if (s->two_class) {
                    float fill = MPCLAMP(
                        0.5f + sample_catmull_rom(&s->sdf_fill, u, v, distance_limit) * threshold_scale, 0.0f, a);
                    float outline = a - fill;
                    for (int c = 0; c < 3; c++)
                        value[c] = fill * s->color1[c] + outline * s->color0[c];
                } else {
                    for (int c = 0; c < 3; c++)
                        value[c] = MPCLAMP(sample_catmull_rom(&s->rgb[c], u, v, 0.0f), 0.0f, a);
                }
            }
            uint8_t *pixel = row + (size_t)x * 4;
            pixel[0] = to_byte(value[2]);
            pixel[1] = to_byte(value[1]);
            pixel[2] = to_byte(value[0]);
            pixel[3] = to_byte(a);
        }
    }

done:
    talloc_free(alpha.v);
    for (int c = 0; c < 3; c++)
        talloc_free(secondary[c].v);
}

int mp_sdf_scaler_source_w(const struct mp_sdf_scaler *s)
{
    return s ? s->alpha.w : 0;
}

int mp_sdf_scaler_source_h(const struct mp_sdf_scaler *s)
{
    return s ? s->alpha.h : 0;
}

int mp_sdf_scaler_padding(const struct mp_sdf_scaler *s)
{
    return s ? s->padding : 0;
}
