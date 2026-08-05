#include <stdlib.h>
#include <string.h>

#include "mpv_talloc.h"
#include "sub/sub_sdf_scale.h"
#include "test_utils.h"

static void make_glyph(uint8_t *image, int w, int h)
{
    memset(image, 0, (size_t)w * h * 4);
    for (int y = 1; y < h - 1; y++) {
        for (int x = 1; x < w - 1; x++) {
            uint8_t *pixel = image + ((size_t)y * w + x) * 4;
            pixel[3] = 255;
            if (x >= 2 && x < w - 2 && y >= 2 && y < h - 2)
                pixel[0] = pixel[1] = pixel[2] = 255;
        }
    }
}

static const uint8_t *pixel_at(const uint8_t *image, int stride, int x, int y)
{
    return image + (size_t)y * stride + (size_t)x * 4;
}

static void test_two_edge_upscale(void)
{
    enum { SW = 8, SH = 8, DW = 64, DH = 64 };
    uint8_t source[SW * SH * 4];
    uint8_t output[DW * DH * 4];
    make_glyph(source, SW, SH);

    struct mp_sdf_scaler *scaler = mp_sdf_scaler_create(NULL, source, SW, SH, SW * 4, true, 4);
    assert_true(scaler);
    assert_int_equal(mp_sdf_scaler_source_w(scaler), 16);
    assert_int_equal(mp_sdf_scaler_source_h(scaler), 16);
    mp_sdf_scaler_render(scaler, output, DW, DH, DW * 4, 0.9f, NULL);

    const uint8_t *outside = pixel_at(output, DW * 4, 12, 32);
    const uint8_t *outline = pixel_at(output, DW * 4, 22, 32);
    const uint8_t *fill = pixel_at(output, DW * 4, 32, 32);
    assert_true(outside[3] < 8);
    assert_true(outline[3] > 240);
    assert_true(outline[0] < 8 && outline[1] < 8 && outline[2] < 8);
    assert_true(fill[3] > 250);
    assert_true(fill[0] > 240 && fill[1] > 240 && fill[2] > 240);

    for (int i = 0; i < DW * DH; i++) {
        const uint8_t *pixel = output + (size_t)i * 4;
        assert_true(pixel[0] <= pixel[3]);
        assert_true(pixel[1] <= pixel[3]);
        assert_true(pixel[2] <= pixel[3]);
    }
    talloc_free(scaler);
}

static void test_area_downscale_preserves_coverage(void)
{
    enum { SW = 8, SH = 8, DW = 8, DH = 8 };
    uint8_t source[SW * SH * 4];
    uint8_t output[DW * DH * 4];
    make_glyph(source, SW, SH);

    struct mp_sdf_scaler *scaler = mp_sdf_scaler_create(NULL, source, SW, SH, SW * 4, false, 4);
    assert_true(scaler);
    mp_sdf_scaler_render(scaler, output, DW, DH, DW * 4, 1.0f, NULL);

    int alpha_sum = 0;
    for (int i = 0; i < DW * DH; i++)
        alpha_sum += output[i * 4 + 3];
    // The padded source is 16x16 and the 6x6 opaque square covers 36 source
    // pixels. Scaling to 8x8 therefore preserves 36 / 4 pixel-coverages.
    assert_true(abs(alpha_sum - 9 * 255) <= 32);
    talloc_free(scaler);
}

static void test_two_layer_shadow_extends_coverage(void)
{
    enum { SW = 8, SH = 8, DW = 96, DH = 96 };
    uint8_t source[SW * SH * 4];
    uint8_t output[DW * DH * 4];
    make_glyph(source, SW, SH);

    struct mp_sdf_scaler *scaler = mp_sdf_scaler_create(NULL, source, SW, SH, SW * 4, false, 8);
    assert_true(scaler);
    struct mp_sdf_shadow_params shadow = {
        .enabled = true,
        .core_sigma = 1.0f,
        .core_grow = 1.0f,
        .core_opacity = 0.7f,
        .spread_enabled = true,
        .spread_sigma = 6.0f,
        .spread_x = 2.0f,
        .spread_y = 3.0f,
        .spread_opacity = 0.3f,
    };
    mp_sdf_scaler_render(scaler, output, DW, DH, DW * 4, 1.0f, &shadow);

    const uint8_t *far = pixel_at(output, DW * 4, 2, 48);
    const uint8_t *halo = pixel_at(output, DW * 4, 32, 48);
    const uint8_t *glyph = pixel_at(output, DW * 4, 48, 48);
    assert_true(far[3] == 0);
    assert_true(halo[3] > 5 && halo[3] < 180);
    assert_true(halo[0] < 2 && halo[1] < 2 && halo[2] < 2);
    assert_true(glyph[3] > 250 && glyph[0] > 240 && glyph[1] > 240 && glyph[2] > 240);
    talloc_free(scaler);
}

int main(void)
{
    test_two_edge_upscale();
    test_area_downscale_preserves_coverage();
    test_two_layer_shadow_extends_coverage();
    return 0;
}
