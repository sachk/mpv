/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License, version 2.1 or later.
 */

#include "libmpv_common.h"

static void wait_for_restart(void)
{
    for (int attempts = 0; attempts < 50; attempts++) {
        mpv_event *event = mpv_wait_event(ctx, 0.1);
        if (event->event_id == MPV_EVENT_PLAYBACK_RESTART)
            return;
        if (event->event_id == MPV_EVENT_END_FILE)
            fail("playback ended before restart\n");
        if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message *message = event->data;
            printf("[%s:%s] %s", message->prefix, message->level, message->text);
            if (message->log_level <= MPV_LOG_LEVEL_ERROR)
                fail("error was logged\n");
        }
    }
    fail("playback did not restart within five seconds\n");
}

static void seek_to(const char *seconds)
{
    const char *cmd[] = { "seek", seconds, "absolute+keyframes", NULL };
    command(cmd);
    wait_for_restart();
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 1;

    ctx = mpv_create();
    if (!ctx)
        return 1;
    atexit(exit_cleanup);

    set_property_string("pause", "yes");
    initialize();
    reload_file(argv[1]);
    set_property_string("pause", "no");
    wait_for_restart();
    seek_to("0.8");
    seek_to("1.4");
    return 0;
}
