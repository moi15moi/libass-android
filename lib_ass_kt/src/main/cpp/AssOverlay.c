#include "AssOverlay.h"

#include <stdlib.h>
#include <time.h>

#include "AssAtlas.h"
#include "AssOverlayGl.h"
#include "AssOverlayLog.h"

// ============================================================================
// EFFECTS_ATLAS: draws subtitles onto the video frames of media3's GlEffect path.
//
// The caller's FBO wraps the decoded video frame's own texture, and the subtitles are drawn into it
// in place: there is no separate output texture or full-frame copy, and a frame without visible
// subtitles costs no GL work at all.
//
// A frame's subtitle bitmaps are packed into atlas pages (AssAtlas.c) - normally one, more only when
// heavy typesetting doesn't fit in a single max-size texture - and drawn with one call per page
// (AssOverlayGl.c).
//
// ass_render_frame runs synchronously, on this call (the GL thread), so every frame is
// frame-accurate, at the cost that a pathologically slow subtitle frame stalls video delivery for as
// long as ass_render_frame and the atlas update take.
//
// Always called under the JVM side's `Ass.lock` (see AssRender.kt's drawOverlayFrame), which guards
// against concurrent track changes: nothing here needs its own synchronization.
// ============================================================================

// An update taking at least this long is logged as a candidate culprit for a slow frame; below it
// is normal jitter.
#define ASS_OVERLAY_SLOW_LOG_MS 2

typedef struct {
    AssOverlayGl gl;
    AssAtlas atlas;
    // The video frame's pixel size: libass's storage and frame size, and the GL viewport.
    int frameW, frameH;
} AssOverlay;

static long long nowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void destroyOverlay(AssOverlay* overlay) {
    if (overlay == NULL) return;
    assOverlayGlRelease(&overlay->gl);
    assAtlasFree(&overlay->atlas);
    free(overlay);
}

static AssOverlay* createOverlay(void) {
    AssOverlay* overlay = (AssOverlay*) calloc(1, sizeof(AssOverlay));
    if (overlay == NULL) {
        LOGE("Failed to allocate the overlay");
        return NULL;
    }
    if (!assOverlayGlInit(&overlay->gl)) {
        destroyOverlay(overlay);
        return NULL;
    }
    overlay->atlas.maxPageSize = overlay->gl.maxTextureSize;
    return overlay;
}

// Brings the atlas and its quads in line with libass's latest output. changed == 0 means the same
// content as last time, so everything is reused as is.
static void updateAtlas(AssOverlay* overlay, const ASS_Image* images, int changed) {
    if (changed == 0) return;
    AssAtlas* atlas = &overlay->atlas;

    // changed == 1 means only positions moved: the uploaded pages stay valid if the bitmaps line up.
    const bool moved = changed == 1 && atlas->pieceCount > 0 && assAtlasMovePieces(atlas, images);
    if (!moved) {
        assAtlasPack(atlas, images);
        if (atlas->pieceCount == 0) return;
        const int uploadedPages = assOverlayGlUpload(&overlay->gl, atlas);
        assAtlasTruncate(atlas, uploadedPages); // fewer only on OOM
        if (atlas->pieceCount == 0) return;
    }
    if (!assOverlayGlSetQuads(&overlay->gl, atlas, overlay->frameW, overlay->frameH)) assAtlasTruncate(atlas, 0);
}

// Called for every video frame, on the GL thread, from AssRender.kt's drawOverlayFrame (itself called
// from AssGlShaderProgram.queueInputFrame). `fbo` wraps the frame's own texture, which the caller
// forwards downstream as is: a frame without visible subtitles costs nothing beyond
// ass_render_frame.
jlong nativeAssOverlayDraw(JNIEnv* env, jclass clazz, jlong handle, jlong render, jlong track,
    jint fbo, jint frameWidth, jint frameHeight, jlong timeMs) {
    if (render == 0 || track == 0) return handle;
    AssOverlay* overlay = (AssOverlay*) handle;
    if (overlay == NULL) {
        overlay = createOverlay();
        if (overlay == NULL) return 0;
    }

    if (frameWidth != overlay->frameW || frameHeight != overlay->frameH) {
        overlay->frameW = frameWidth;
        overlay->frameH = frameHeight;
        // libass renders at the frame's own size, so storage and frame size are the same.
        ass_set_storage_size((ASS_Renderer*) render, frameWidth, frameHeight);
        ass_set_frame_size((ASS_Renderer*) render, frameWidth, frameHeight);
        LOGI("ass_set_storage_size/ass_set_frame_size(%d, %d)", frameWidth, frameHeight);
    }

    const long long start = nowMs();
    int changed = 0;
    const ASS_Image* images = ass_render_frame((ASS_Renderer*) render, (ASS_Track*) track, timeMs, &changed);
    const long long rendered = nowMs();
    updateAtlas(overlay, images, changed);
    const long long updated = nowMs();
    if (updated - start >= ASS_OVERLAY_SLOW_LOG_MS) {
        LOGW("timeMs=%lld (changed=%d): ass_render_frame %lldms, atlas update %lldms (pieces=%d, pages=%d)",
            (long long) timeMs, changed, rendered - start, updated - rendered, overlay->atlas.pieceCount, overlay->atlas.pageCount);
    }

    // The atlas outlives changed == 0 frames, but each call's FBO wraps a new video frame that doesn't
    // have the subtitles on it yet.
    if (overlay->atlas.pieceCount > 0) {
        assOverlayGlDraw(&overlay->gl, &overlay->atlas, (GLuint) fbo, overlay->frameW, overlay->frameH);
    }
    return (jlong) overlay;
}

void nativeAssOverlayRelease(JNIEnv* env, jclass clazz, jlong handle) {
    destroyOverlay((AssOverlay*) handle);
}
