#ifndef LIBASS_ANDROID_ASS_OVERLAY_H
#define LIBASS_ANDROID_ASS_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>
#include <jni.h>
#include "GLES3/gl3.h"

// Native side of AssRenderType.EFFECTS_ATLAS.

// One packed subtitle piece. Position/color are carried raw (not yet clip-space/UV-normalized):
// that conversion depends on frameW/frameH and the atlas texture's current capacity, so
// it happens at vertex-build time rather than being baked in while packing.
typedef struct {
    int atlasX, atlasY;
    int dstX, dstY, w, h;
    uint32_t color; // libass RGBA, A being transparency
    // Source pixels. Only dereferenced during the ass_render_frame call that produced them; kept
    // after that purely for pointer comparison in assOverlayUpdatePositions.
    const unsigned char* bitmap;
    int stride;
} AssOverlayPiece;

// One atlas page: a GPU texture holding a CONTIGUOUS run [firstPiece, firstPiece + pieceCount) of
// ctx->pieces[] (i.e. of the libass image list). A heavy typesetting frame can easily produce more
// bitmap area than fits in a single max-size texture; rather than silently dropping whatever
// doesn't fit, packing spills into another page. Pages are drawn in order, and each holds a
// contiguous list range, so the overall paint order (and therefore the "over" blending) is exactly
// libass's list order regardless of how many pages were needed.
typedef struct {
    GLuint tex;
    int capW, capH; // allocated GPU texture capacity (grows in steps, rarely reallocated)
    int usedW, usedH; // tight packed size actually used by the last pack
    int firstPiece, pieceCount;
} AssOverlayPage;

typedef struct {
    float x, y; // clip space
    float u, v; // atlas texture coordinates
    uint8_t r, g, b, a; // normalized to [0, 1] by the attribute pointer
} AssOverlayVertex;

typedef struct {
    // GL objects, created lazily on first call.
    GLuint atlasProgram;
    GLint atlasAPosition;
    GLint atlasATexCoord;
    GLint atlasAColor;
    GLuint atlasVbo;
    int maxAtlasSize;
    bool bIsGles3;   // context is GLES 3.0+: R8 textures + PBO uploads (see assOverlayUpload)
    GLuint pbo;  // GL_PIXEL_UNPACK_BUFFER reused for every page upload (GLES3 only)
    size_t pboSize; // pbo's allocation size: grow-only, so orphaning always asks for the same size

    // Pages [0, pageCount) are in use; [pageCount, pageCap) keep their textures around for reuse.
    AssOverlayPage* pages;
    int pageCap;
    int pageCount;

    // The video frame's own pixel size == libass storage and frame size == GL viewport. Only
    // re-applied via ass_set_* when it actually changes.
    int frameW, frameH;

    // CPU-side staging for one page at a time (GLES2, or if mapping the PBO fails).
    unsigned char* atlasBuf;
    int atlasBufCap;

    AssOverlayPiece* pieces;
    int pieceCap;
    int pieceCount; // current visible piece count; sticky across changed==0 frames

    AssOverlayVertex* vertexBuf; // 6 vertices (two triangles) per piece
    int vertexCap;
} AssOverlayContext;

jlong nativeAssOverlayDraw(JNIEnv* env, jclass clazz, jlong overlay, jlong render, jlong track,
    jint fbo, jint frameWidth, jint frameHeight, jlong timeMs);

void nativeAssOverlayRelease(JNIEnv* env, jclass clazz, jlong overlay);

#endif // LIBASS_ANDROID_ASS_OVERLAY_H
