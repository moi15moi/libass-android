// libass set up the way the app sets it up (AssKt.c), for the tools that render a track themselves.
#pragma once

#include <ass/ass.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    ASS_Library* library;
    ASS_Renderer* renderer;
    ASS_Track* track;
    long long endMs; // end of the track's last event
} LibassTrack;

// Only libass errors: warnings (e.g. about missing glyphs) would flood the output.
static void libassQuietMessages(int level, const char* fmt, va_list args, void* data) {
    (void) data;
    if (level > 1) return;
    fprintf(stderr, "libass: ");
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
}

// Loads a track, with the app's fontconfig setup plus the fonts in `fontsDir` (the container's
// attachments, which the app adds with ass_add_font). Exits on error.
static LibassTrack openLibassTrack(const char* trackPath, const char* fontsDir) {
    LibassTrack t;
    t.library = ass_library_init();
    if (t.library == NULL) {
        fprintf(stderr, "ass_library_init failed\n");
        exit(2);
    }
    ass_set_message_cb(t.library, libassQuietMessages, NULL);
    ass_set_extract_fonts(t.library, 1);
    ass_set_fonts_dir(t.library, fontsDir);
    t.renderer = ass_renderer_init(t.library);
    if (t.renderer == NULL) {
        fprintf(stderr, "ass_renderer_init failed\n");
        exit(2);
    }
    ass_set_fonts(t.renderer, NULL, "sans-serif", ASS_FONTPROVIDER_FONTCONFIG, NULL, 1);
    t.track = ass_read_file(t.library, (char*) trackPath, NULL);
    if (t.track == NULL) {
        fprintf(stderr, "%s: cannot read the track\n", trackPath);
        exit(2);
    }
    t.endMs = 0;
    for (int i = 0; i < t.track->n_events; i++) {
        long long eventEnd = t.track->events[i].Start + t.track->events[i].Duration;
        if (eventEnd > t.endMs) t.endMs = eventEnd;
    }
    return t;
}

static void closeLibassTrack(LibassTrack* t) {
    ass_free_track(t->track);
    ass_renderer_done(t->renderer);
    ass_library_done(t->library);
}

// Presentation time of frame `frameIdx` at `fps`, in whole milliseconds like a player's clock.
static long long frameTimeMs(long long frameIdx, double fps) {
    return (long long) (frameIdx * 1000.0 / fps + 0.5);
}
