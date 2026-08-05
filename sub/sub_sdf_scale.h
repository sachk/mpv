/*
 * High-quality resampling for decoded bitmap subtitles.
 *
 * This file is part of mpv and is licensed under the GNU LGPL v2.1 or later.
 */
#ifndef MP_SUB_SDF_SCALE_H
#define MP_SUB_SDF_SCALE_H

#include <stdbool.h>
#include <stdint.h>

struct mp_sdf_scaler;
struct mp_sdf_shadow_params {
    bool enabled;
    float core_sigma;
    float core_grow;
    float core_opacity;
    bool spread_enabled;
    float spread_sigma;
    float spread_grow;
    float spread_x;
    float spread_y;
    float spread_opacity;
    bool dither;
};

struct mp_sdf_scaler *mp_sdf_scaler_create(
    void *ta_parent, const uint8_t *src, int w, int h, int stride, bool clamp_far_field, int padding);

void mp_sdf_scaler_render(struct mp_sdf_scaler *scaler, uint8_t *dst, int w, int h, int stride, float softness,
    const struct mp_sdf_shadow_params *shadow);

int mp_sdf_scaler_source_w(const struct mp_sdf_scaler *scaler);
int mp_sdf_scaler_source_h(const struct mp_sdf_scaler *scaler);
int mp_sdf_scaler_padding(const struct mp_sdf_scaler *scaler);

#endif
