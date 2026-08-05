#include <stdlib.h>

#include "sub/osd.h"
#include "sub/sub_image_geometry.h"
#include "sub/sub_image_segmentation.h"
#include "test_utils.h"

static struct sub_bitmap bitmap(int x, int y, int w, int h)
{
    return (struct sub_bitmap) {
        .w = w,
        .h = h,
        .x = x,
        .y = y,
        .dw = w,
        .dh = h,
    };
}

static int ink_bottom(const struct sub_bitmaps *imgs, int extend)
{
    int bottom = INT_MIN;
    for (int i = 0; i < imgs->num_parts; i++) {
        const struct sub_bitmap *part = &imgs->parts[i];
        int pad = part->h > 0 ? lrint(extend * part->dh / (double)part->h) : 0;
        bottom = MPMAX(bottom, part->y + part->dh - pad);
    }
    return bottom;
}

static int ink_horizontal_center(const struct sub_bitmaps *imgs)
{
    int left = INT_MAX;
    int right = INT_MIN;
    for (int i = 0; i < imgs->num_parts; i++) {
        left = MPMIN(left, imgs->parts[i].x);
        right = MPMAX(right, imgs->parts[i].x + imgs->parts[i].dw);
    }
    return (left + right) / 2;
}

static void assert_rect_matches(struct mp_rect actual, struct mp_rect expected)
{
    assert_int_equal(actual.x0, expected.x0);
    assert_int_equal(actual.y0, expected.y0);
    assert_int_equal(actual.x1, expected.x1);
    assert_int_equal(actual.y1, expected.y1);
}

static void test_authored_y_normalizes_to_one_anchor(void)
{
    struct mp_rect visible = { 0, 0, 1920, 1080 };
    struct sub_bitmap low_parts[] = {
        bitmap(700, 760, 120, 40),
        bitmap(830, 770, 80, 30),
    };
    struct sub_bitmap high_parts[] = {
        bitmap(700, 260, 120, 40),
        bitmap(830, 270, 80, 30),
    };
    struct sub_bitmaps low = { .parts = low_parts, .num_parts = 2 };
    struct sub_bitmaps high = { .parts = high_parts, .num_parts = 2 };
    int low_relative_y = low_parts[1].y - low_parts[0].y;
    int high_relative_y = high_parts[1].y - high_parts[0].y;

    mp_image_subtitle_reposition_all(&low, 0, visible, 50);
    mp_image_subtitle_reposition_all(&high, 0, visible, 50);

    assert_int_equal(ink_bottom(&low, 0), 540);
    assert_int_equal(ink_bottom(&high, 0), 540);
    assert_int_equal(low_parts[1].y - low_parts[0].y, low_relative_y);
    assert_int_equal(high_parts[1].y - high_parts[0].y, high_relative_y);
}

static void test_bottom_anchored_multipart_scale(void)
{
    struct mp_rect visible = { 0, 0, 1920, 1080 };
    struct sub_bitmap parts[] = {
        bitmap(700, 500, 100, 40),
        bitmap(820, 510, 60, 30),
    };
    struct sub_bitmaps imgs = { .parts = parts, .num_parts = 2 };
    int original_bottom = ink_bottom(&imgs, 0);
    int original_dx = parts[1].x - parts[0].x;

    mp_image_subtitle_scale_all(&imgs, 0, 1.5f, visible);

    assert_int_equal(parts[0].dw, 150);
    assert_int_equal(parts[0].dh, 60);
    assert_int_equal(parts[1].dw, 90);
    assert_int_equal(parts[1].dh, 45);
    assert_true(labs((parts[1].x - parts[0].x) - lrint(original_dx * 1.5)) <= 1);
    assert_int_equal(ink_bottom(&imgs, 0), original_bottom);
}

static void test_padding_noops_and_top_clamp(void)
{
    struct mp_rect visible = { 0, 0, 800, 800 };
    struct sub_bitmap padded_part = bitmap(100, 200, 110, 50);
    struct sub_bitmaps padded = { .parts = &padded_part, .num_parts = 1 };
    mp_image_subtitle_reposition_all(&padded, 5, visible, 50);
    assert_int_equal(ink_bottom(&padded, 5), 400);

    struct sub_bitmaps empty = { 0 };
    mp_image_subtitle_reposition_all(&empty, 5, visible, 20);
    mp_image_subtitle_scale_all(&empty, 5, 1.5f, visible);

    struct sub_bitmap unchanged_part = bitmap(50, 60, 70, 80);
    struct sub_bitmap before = unchanged_part;
    struct sub_bitmaps unchanged = { .parts = &unchanged_part, .num_parts = 1 };
    mp_image_subtitle_scale_all(&unchanged, 0, 1.0f, visible);
    assert_memcmp(&unchanged_part, &before, sizeof(before));

    struct sub_bitmap crossing_part = bitmap(100, 10, 100, 90);
    struct sub_bitmaps crossing = { .parts = &crossing_part, .num_parts = 1 };
    mp_image_subtitle_scale_all(&crossing, 0, 2.0f, visible);
    assert_int_equal(crossing_part.y, visible.y0);

    struct sub_bitmap oversized_part = bitmap(100, 0, 100, 600);
    struct sub_bitmaps oversized = { .parts = &oversized_part, .num_parts = 1 };
    mp_image_subtitle_scale_all(&oversized, 0, 2.0f, visible);
    assert_int_equal(oversized_part.y, visible.y0);
    assert_true(oversized_part.dh > mp_rect_h(visible));
}

static void test_full_output_allows_letterbox_and_pillarbox_placement(void)
{
    struct mp_rect output = { 0, 0, 1920, 1080 };
    struct sub_bitmap letterbox_part = bitmap(800, 800, 320, 70);
    struct sub_bitmaps letterbox = { .parts = &letterbox_part, .num_parts = 1 };
    mp_image_subtitle_reposition_all(&letterbox, 0, output, 95);
    assert_int_equal(ink_bottom(&letterbox, 0), 1026);
    assert_true(ink_bottom(&letterbox, 0) > 945);

    struct sub_bitmap pillarbox_part = bitmap(500, 700, 920, 80);
    struct sub_bitmaps pillarbox = { .parts = &pillarbox_part, .num_parts = 1 };
    mp_image_subtitle_scale_all(&pillarbox, 0, 1.5f, output);
    assert_true(pillarbox_part.x < 420);
    assert_true(pillarbox_part.x + pillarbox_part.dw > 1500);

    struct sub_bitmap wider_than_output_part = bitmap(300, 700, 1320, 80);
    struct sub_bitmaps wider_than_output = { .parts = &wider_than_output_part, .num_parts = 1 };
    mp_image_subtitle_scale_all(&wider_than_output, 0, 2.0f, output);
    assert_int_equal(wider_than_output_part.x, output.x0);
    assert_true(wider_than_output_part.dw > mp_rect_w(output));
}

static void test_video_viewport_excludes_black_bars(void)
{
    struct mp_osd_res output = {
        .w = 1920,
        .h = 1080,
        .ml = 160,
        .mr = 160,
        .mt = 90,
        .mb = 90,
    };
    assert_rect_matches(mp_image_subtitle_viewport(output, true), ((struct mp_rect) { 0, 0, 1920, 1080 }));
    assert_rect_matches(mp_image_subtitle_viewport(output, false), ((struct mp_rect) { 160, 90, 1760, 990 }));

    output.ml = output.mr = 1000;
    assert_rect_matches(mp_image_subtitle_viewport(output, false), ((struct mp_rect) { 0, 0, 1920, 1080 }));
}

static void test_reposition_centers_and_keeps_100_continuous(void)
{
    struct mp_rect visible = { 160, 90, 1760, 990 };
    struct sub_bitmap part = bitmap(300, 400, 400, 80);
    struct sub_bitmaps imgs = { .parts = &part, .num_parts = 1 };

    mp_image_subtitle_reposition_all(&imgs, 0, visible, 100);

    assert_int_equal(part.x + part.dw / 2, (visible.x0 + visible.x1) / 2);
    assert_int_equal(ink_bottom(&imgs, 0), visible.y1);
}

static void test_distant_groups_compact_per_source_line(void)
{
    struct mp_rect visible = { 0, 0, 1200, 800 };
    struct sub_bitmap parts[] = {
        bitmap(100, 600, 100, 40),
        bitmap(900, 600, 100, 40),
        bitmap(300, 670, 220, 40),
    };
    struct sub_bitmaps imgs = { .parts = parts, .num_parts = 3 };

    mp_image_subtitle_reposition_all(&imgs, 0, visible, 100);

    assert_true(parts[0].x < parts[1].x);
    assert_int_equal(parts[1].x - (parts[0].x + parts[0].w), 20);
    assert_int_equal(ink_horizontal_center(&imgs), (visible.x0 + visible.x1) / 2);
    assert_int_equal(parts[2].y - parts[0].y, 70);
    assert_int_equal(ink_bottom(&imgs, 0), visible.y1);
}

static void test_pgs_multipart_speakers_preserve_authored_rows(void)
{
    struct sub_bitmap parts[] = {
        bitmap(100, 500, 100, 30),
        bitmap(900, 500, 100, 30),
        bitmap(110, 540, 120, 30),
        bitmap(890, 540, 120, 30),
    };
    struct sub_bitmaps imgs = { .parts = parts, .num_parts = 4 };

    mp_image_subtitle_reposition_all(&imgs, 0, (struct mp_rect) { 0, 0, 1200, 800 }, 100);

    assert_int_equal(parts[1].x - (parts[0].x + parts[0].w), 15);
    assert_int_equal(parts[3].x - (parts[2].x + parts[2].w), 15);
    assert_int_equal(parts[0].y, parts[1].y);
    assert_int_equal(parts[2].y, parts[3].y);
    assert_int_equal(parts[2].y - parts[0].y, 40);
    assert_true(labs(ink_horizontal_center(&imgs) - 600) <= 1);
    assert_int_equal(ink_bottom(&imgs, 0), 800);
}

static void fill_rect(uint32_t *pixels, int stride, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++)
            pixels[y * stride + x] = 0xFFFFFFFFu;
    }
}

static void test_vobsub_pixel_groups_and_multiline_layout(void)
{
    enum { W = 400, H = 100, EXTEND = 5 };
    uint32_t pixels[W * H] = { 0 };
    fill_rect(pixels, W, 15, 10, 80, 35);
    fill_rect(pixels, W, 20, 55, 90, 80);
    fill_rect(pixels, W, 300, 10, 370, 35);
    fill_rect(pixels, W, 290, 55, 380, 80);

    struct sub_bitmap source = bitmap(50, 500, W, H);
    source.bitmap = pixels;
    source.stride = W * 4;
    struct sub_bitmap split[4] = { 0 };
    int occupied[W];
    int count = mp_image_subtitle_split_columns(
        &source, EXTEND, split, MP_ARRAY_SIZE(split), occupied, MP_ARRAY_SIZE(occupied));
    assert_int_equal(count, 2);
    assert_true(split[0].x < split[1].x);
    assert_int_equal(split[0].y, source.y);
    assert_int_equal(split[1].y, source.y);
    assert_int_equal(split[0].h, source.h);
    assert_int_equal(split[1].h, source.h);

    struct sub_bitmaps imgs = { .parts = split, .num_parts = count };
    mp_image_subtitle_reposition_all(&imgs, EXTEND, (struct mp_rect) { 0, 0, 1200, 800 }, 100);
    int ink_gap = split[1].x + EXTEND - (split[0].x + split[0].w - EXTEND);
    assert_int_equal(ink_gap, (H - EXTEND * 2) / 2);
}

static void test_bitmap_text_scale_detects_alpha_row_height(void)
{
    enum { W = 300, H = 100 };
    uint32_t pixels[W * H] = { 0 };
    fill_rect(pixels, W, 20, 10, 280, 40);
    fill_rect(pixels, W, 30, 60, 270, 90);

    struct sub_bitmap part = bitmap(100, 700, W, H);
    part.bitmap = pixels;
    part.stride = W * 4;
    struct sub_bitmaps imgs = {
        .format = SUBBITMAP_BGRA,
        .parts = &part,
        .num_parts = 1,
    };
    struct mp_rect visible = { 0, 0, 1920, 1000 };

    float normal = mp_image_subtitle_text_scale(&imgs, 0, 1.0f, visible);
    float larger = mp_image_subtitle_text_scale(&imgs, 0, 1.25f, visible);

    assert_true(fabsf(normal - 52.0f / 30.0f) < 0.01f);
    assert_true(fabsf(larger - 65.0f / 30.0f) < 0.01f);
}

int main(void)
{
    test_authored_y_normalizes_to_one_anchor();
    test_bottom_anchored_multipart_scale();
    test_padding_noops_and_top_clamp();
    test_full_output_allows_letterbox_and_pillarbox_placement();
    test_video_viewport_excludes_black_bars();
    test_reposition_centers_and_keeps_100_continuous();
    test_distant_groups_compact_per_source_line();
    test_pgs_multipart_speakers_preserve_authored_rows();
    test_vobsub_pixel_groups_and_multiline_layout();
    test_bitmap_text_scale_detects_alpha_row_height();
    return 0;
}
