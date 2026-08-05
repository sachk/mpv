#include "sub/sub_ass_color.h"
#include "test_utils.h"

static void test_text_and_inline_colours_are_overridden(void)
{
    ASS_Style style = {
        .PrimaryColour = 0xFFFF00A0u,
        .SecondaryColour = 0x00FFFF80u,
        .OutlineColour = 0x00000020u,
        .BackColour = 0x10101040u,
    };
    uint32_t desired = 0x33CC99FFu;

    assert_int_equal(mp_ass_text_color_override(style.PrimaryColour, desired, &style, 1), 0x33CC99A0u);
    assert_int_equal(mp_ass_text_color_override(style.SecondaryColour, desired, &style, 1), 0x33CC9980u);
    assert_int_equal(mp_ass_text_color_override(0x12345670u, desired, &style, 1), 0x33CC9970u);
}

static void test_outline_and_background_colours_are_preserved(void)
{
    ASS_Style style = {
        .PrimaryColour = 0xFFFFFFFFu,
        .SecondaryColour = 0xFFFFFFFFu,
        .OutlineColour = 0x00000020u,
        .BackColour = 0x10101040u,
    };

    assert_int_equal(mp_ass_text_color_override(style.OutlineColour, 0xFF0000FFu, &style, 1), style.OutlineColour);
    assert_int_equal(mp_ass_text_color_override(style.BackColour, 0xFF0000FFu, &style, 1), style.BackColour);
}

int main(void)
{
    test_text_and_inline_colours_are_overridden();
    test_outline_and_background_colours_are_preserved();
    return 0;
}
