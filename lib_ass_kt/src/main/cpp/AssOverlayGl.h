#ifndef LIBASS_ANDROID_ASS_OVERLAY_GL_H
#define LIBASS_ANDROID_ASS_OVERLAY_GL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "AssAtlas.h"
#include "AssOverlayProfile.h"
#include "GLES3/gl3.h"

// The texture holding one atlas page. Its capacity only grows, so it is rarely reallocated.
typedef struct {
    GLuint id;
    int capW, capH;
} AssOverlayTexture;

typedef struct {
    float x, y; // clip space
    float u, v; // texture coordinates
    uint8_t r, g, b, a; // normalized to [0, 1] by the attribute pointer
} AssOverlayVertex;

// The GL side of the overlay: shader program, page textures, uploads and quads. GL thread only, with
// the context current.
typedef struct {
    int maxTextureSize;
    bool isGles3; // R8 textures and PBO uploads; GLES2 gets ALPHA textures and client-memory uploads

    GLuint program;
    GLint aPosition, aTexCoord, aColor;

    AssOverlayTexture* textures; // textures[i] holds atlas page i; spare ones are kept for reuse
    int textureCap;

    // The GLES3 upload buffer. Grow-only and never orphaned: on ANGLE, orphaning a multi-MB buffer
    // allocates and page-faults new storage on every upload (10-18 ms). Instead it's mapped
    // unsynchronized once the fence of its previous upload has signaled, which that upload normally
    // has long done by the next changed frame: one buffer is enough.
    GLuint pbo;
    size_t pboSize;
    GLsync pboFence;
    // For when there is no PBO (GLES2) or it can't be mapped.
    unsigned char* staging;
    size_t stagingCap;

    GLuint vbo;
    int vboCap; // in vertices
    AssOverlayVertex* vertices; // a quad (6 vertices) per atlas piece
    int vertexCap;
} AssOverlayGl;

// Builds the shader program and reads the context's limits. Returns false on failure.
bool assOverlayGlInit(AssOverlayGl* gl);
void assOverlayGlRelease(AssOverlayGl* gl);

// Uploads every atlas page into its texture. Returns how many pages were uploaded: fewer than the
// atlas has only on OOM.
int assOverlayGlUpload(AssOverlayGl* gl, const AssAtlas* atlas, AssOverlayProfile* profile);

// Rebuilds the quads that draw the atlas pieces onto a frameW x frameH frame. Returns false on OOM.
bool assOverlayGlSetQuads(AssOverlayGl* gl, const AssAtlas* atlas, int frameW, int frameH);

// Draws the quads onto `fbo`, one call per page, then restores the GL state media3 expects.
void assOverlayGlDraw(const AssOverlayGl* gl, const AssAtlas* atlas, GLuint fbo, int frameW, int frameH);

#endif // LIBASS_ANDROID_ASS_OVERLAY_GL_H
