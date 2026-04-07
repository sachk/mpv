/*
 * This file is part of mpv.
 */

#include "common/codecs.h"
#include "filters/f_decoder_wrapper.h"

static struct mp_decoder *create(struct mp_filter *parent,
                                 struct mp_codec_params *codec,
                                 const char *decoder)
{
    return NULL;
}

static void add_decoders(struct mp_decoder_list *list)
{
    mp_add_decoder(list, "h264", "starfish", "LG webOS Starfish packet sink");
    mp_add_decoder(list, "hevc", "starfish", "LG webOS Starfish packet sink");
    mp_add_decoder(list, "vp9", "starfish", "LG webOS Starfish packet sink");
    mp_add_decoder(list, "av1", "starfish", "LG webOS Starfish packet sink");
}

const struct mp_decoder_fns vd_starfish = {
    .create = create,
    .add_decoders = add_decoders,
};
