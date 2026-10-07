#ifndef LIBASS_ANDROID_ASS_OVERLAY_PROFILE_H
#define LIBASS_ANDROID_ASS_OVERLAY_PROFILE_H

#include <stdbool.h>
#include <time.h>

// Define to also log, for every slow frame, how long each stage of the atlas update took and how
// many texels it moved. It adds clock reads and a pass over the pieces.
#define ASS_OVERLAY_PROFILE

#ifdef ASS_OVERLAY_PROFILE
#define ASS_OVERLAY_PROFILING true
#else
#define ASS_OVERLAY_PROFILING false
#endif

typedef enum {
    ASS_PROFILE_PACK,
    ASS_PROFILE_TEXTURE_ALLOC,
    ASS_PROFILE_MAP,
    ASS_PROFILE_FILL,
    ASS_PROFILE_UPLOAD, // unmap + glTexSubImage2D
    ASS_PROFILE_VERTICES,
    ASS_PROFILE_STAGE_COUNT
} AssProfileStage;

// The last atlas update's stage times, in microseconds, and texel counts.
typedef struct {
    long long stageUs[ASS_PROFILE_STAGE_COUNT];
    long long bitmapTexels; // what the pages store
    long long usedTexels; // what was uploaded: the pages' packed areas
    long long capacityTexels; // the page textures' allocated sizes
} AssOverlayProfile;

static inline long long assProfileNowUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

// Starts and stops timing a stage. Both compile to nothing without ASS_OVERLAY_PROFILE.
static inline long long assProfileStart(void) {
    return ASS_OVERLAY_PROFILING ? assProfileNowUs() : 0;
}

static inline void assProfileStop(AssOverlayProfile* profile, AssProfileStage stage, long long start) {
    if (ASS_OVERLAY_PROFILING) profile->stageUs[stage] += assProfileNowUs() - start;
}

#endif // LIBASS_ANDROID_ASS_OVERLAY_PROFILE_H
