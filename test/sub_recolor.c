#include "test_utils.h"
#include "sub/sub_recolor.h"

#define TARGET 0xFF00A4DCu  // opaque blue
#define ALPHA(c) ((c) >> 24)
#define RED(c) (((c) >> 16) & 0xFF)
#define BLUE(c) ((c) & 0xFF)

// A VOBSUB-style palette: a handful of indices, no antialiasing ramp to speak
// of, plus one color that is deliberately not on the outline-to-fill line.
static void test_vobsub(void)
{
    enum { TRANSPARENT, OUTLINE, FILL, MIDTONE, SIGN, N };
    uint32_t pal[N] = {
        [TRANSPARENT] = 0x00000000,
        [OUTLINE]     = 0xFF000000,
        [FILL]        = 0xFFFFFF00,  // yellow
        [MIDTONE]     = 0xFF808000,  // exactly halfway along black -> yellow
        [SIGN]        = 0xFFFF0000,  // red: nowhere near that line
    };
    uint32_t before[N];
    memcpy(before, pal, sizeof(pal));

    // Occupancy decides the fill. The transparent background dominates the
    // pixel count and must not win.
    int counts[N] = {
        [TRANSPARENT] = 100000, [OUTLINE] = 400, [FILL] = 900,
        [MIDTONE] = 120, [SIGN] = 80,
    };
    struct mp_sub_recolor opts = {
        .color = TARGET,
        .mode = MP_SUB_RECOLOR_REPLACE,
        .ink_threshold = 16,
    };

    assert_true(mp_sub_recolor_palette(pal, N, counts, &opts));

    // Fully transparent entries are never touched.
    assert_int_equal(pal[TRANSPARENT], before[TRANSPARENT]);
    // The outline stays put when no outline color was requested.
    assert_int_equal(pal[OUTLINE], before[OUTLINE]);
    // The fill lands exactly on the requested color.
    assert_int_equal(pal[FILL], TARGET);
    // A deliberate second color is left alone rather than clobbered.
    assert_int_equal(pal[SIGN], before[SIGN]);

    // The midtone is on the ramp, so it moves, but only partway.
    assert_true(pal[MIDTONE] != before[MIDTONE]);
    assert_true(pal[MIDTONE] != TARGET);
    assert_true(BLUE(pal[MIDTONE]) > RED(pal[MIDTONE]));

    for (int n = 0; n < N; n++)
        assert_int_equal(ALPHA(pal[n]), ALPHA(before[n]));
}

// A PGS-style palette: a long antialiasing ramp from the outline to the fill,
// which is really glyph coverage misfiled as color.
static void make_ramp(uint32_t *pal, int n)
{
    pal[0] = 0x00000000;
    for (int i = 1; i < n; i++) {
        uint32_t v = (i - 1) * 255 / (n - 2);
        pal[i] = 0xFF000000u | v << 16 | v << 8 | v;
    }
}

static void test_pgs_ramp_replace(void)
{
    enum { N = 65 };
    uint32_t pal[N], before[N];
    make_ramp(pal, N);
    memcpy(before, pal, sizeof(pal));

    int counts[N] = {0};
    counts[0] = 100000;
    for (int i = 1; i < N; i++)
        counts[i] = 10;
    counts[N - 1] = 5000;  // white is the fill

    struct mp_sub_recolor opts = {
        .color = TARGET,
        .mode = MP_SUB_RECOLOR_REPLACE,
        .ink_threshold = 16,
    };
    assert_true(mp_sub_recolor_palette(pal, N, counts, &opts));

    assert_int_equal(pal[0], before[0]);
    assert_int_equal(pal[1], before[1]);   // darkest entry is the outline
    assert_int_equal(pal[N - 1], TARGET);

    // Coverage has to stay monotonic, or the glyph edges get lumpy.
    for (int i = 2; i < N; i++)
        assert_true(BLUE(pal[i]) >= BLUE(pal[i - 1]));

    for (int i = 0; i < N; i++)
        assert_int_equal(ALPHA(pal[i]), ALPHA(before[i]));
}

// retint keeps each entry's own lightness. On an already neutral ramp, asking
// for a neutral target must therefore round-trip back to the input: this is
// what stops the text from visibly fattening or thinning.
static void test_pgs_ramp_retint(void)
{
    enum { N = 65 };
    uint32_t pal[N], before[N];
    make_ramp(pal, N);
    memcpy(before, pal, sizeof(pal));

    int counts[N] = {0};
    counts[0] = 100000;
    for (int i = 1; i < N; i++)
        counts[i] = 10;
    counts[N - 1] = 5000;

    struct mp_sub_recolor opts = {
        .color = 0xFF808080,
        .mode = MP_SUB_RECOLOR_RETINT,
        .ink_threshold = 16,
    };
    mp_sub_recolor_palette(pal, N, counts, &opts);
    for (int i = 0; i < N; i++) {
        assert_true(abs((int)RED(pal[i]) - (int)RED(before[i])) <= 1);
        assert_int_equal(ALPHA(pal[i]), ALPHA(before[i]));
    }

    // With a colored target the same entries gain hue but keep their level.
    // The tint fades out with coverage, so only the upper half of the ramp is
    // required to be visibly blue; near the outline it is achromatic by design.
    memcpy(pal, before, sizeof(pal));
    opts.color = TARGET;
    assert_true(mp_sub_recolor_palette(pal, N, counts, &opts));
    for (int i = 2; i < N; i++)
        assert_true(BLUE(pal[i]) >= RED(pal[i]));
    for (int i = N / 2; i < N; i++)
        assert_true(BLUE(pal[i]) > RED(pal[i]));
}

static void test_disabled_and_degenerate(void)
{
    uint32_t pal[2] = {0x00000000, 0xFF000000};
    uint32_t before[2];
    memcpy(before, pal, sizeof(pal));
    int counts[2] = {1000, 200};

    // No target color means the feature is off.
    struct mp_sub_recolor opts = {.ink_threshold = 16};
    assert_false(mp_sub_recolor_palette(pal, 2, counts, &opts));
    assert_memcmp(pal, before, sizeof(pal));

    // Only an outline and nothing to use as a fill: leave it all alone rather
    // than guess.
    opts.color = TARGET;
    assert_false(mp_sub_recolor_palette(pal, 2, counts, &opts));
    assert_memcmp(pal, before, sizeof(pal));
}

int main(void)
{
    test_vobsub();
    test_pgs_ramp_replace();
    test_pgs_ramp_retint();
    test_disabled_and_degenerate();
    return 0;
}
