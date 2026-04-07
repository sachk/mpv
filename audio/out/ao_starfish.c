/*
 * This file is part of mpv.
 */

#include "audio/out/ao.h"

static int init(struct ao *ao)
{
    return -1;
}

const struct ao_driver audio_out_starfish = {
    .description = "LG webOS Starfish",
    .name = "starfish",
    .init = init,
};
