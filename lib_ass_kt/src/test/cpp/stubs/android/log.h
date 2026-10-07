// Host stand-in for the NDK's <android/log.h>, so AssOverlay.c compiles off-device. Messages go to
// stderr when ASS_OVERLAY_LOG is set in the environment, and are dropped otherwise.
#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6

static inline int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    (void) prio;
    if (getenv("ASS_OVERLAY_LOG") == NULL) return 0;
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
    return 0;
}
