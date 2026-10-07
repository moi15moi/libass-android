// End-to-end test of the overlay on a headless GLES 3 context (Mesa's surfaceless EGL platform).
//
// usage: test_gl [-m max-atlas-size] [-2] [-n frames] [-r fps] <track.ass> <fonts-dir> <width>x<height>
//   -m  report a GL_MAX_TEXTURE_SIZE no larger than this, to force multi-page frames
//   -2  report GLES 2, for its upload path (ALPHA texture, client-memory upload)
//   -n  stop after this many frames
//   -r  frame rate (default 23.976)
//
// Draws the track frame by frame with nativeAssOverlayDraw into an RGBA8 texture standing in for the
// video frame, reads every frame back, and compares it with the same libass images composited on the
// CPU. Exits with 1 if any channel strays further than blending rounding allows. Prints the texels
// uploaded per repack, the pages drawn, and a hash of every frame read back, so two builds - e.g. of
// an older overlay, see run.sh compare - can be checked for identical output.
//
// Only the JNI entry points are used: the test steers and watches the overlay through the GL calls
// below, wrapped at link time (-Wl,--wrap), so it builds against any version of the overlay.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "AssOverlay.h"
#include "GLES3/gl3.h"
#include "libass_track.h"

// The CPU compositing rounds the way llvmpipe blends into an 8-bit target, but not bit-exactly: over
// a deep stack of layers the two drift apart by up to 3 on the test videos. A misplaced or missing
// piece, or a wrong paint order, is off by far more.
#define MAX_CHANNEL_DIFF 4

// The "video frame" background: exact 8-bit values, so clearing doesn't round.
static const unsigned char kBackground[3] = {64, 128, 192};

static int gMaxTextureSize; // 0: the GPU's own
static bool gReportGles2;
static int gDrawCalls; // since the last reset
static long long gUploadedTexels;

void __real_glGetIntegerv(GLenum pname, GLint* data);
void __wrap_glGetIntegerv(GLenum pname, GLint* data) {
    __real_glGetIntegerv(pname, data);
    if (pname == GL_MAX_TEXTURE_SIZE && gMaxTextureSize > 0 && *data > gMaxTextureSize) *data = gMaxTextureSize;
}

const GLubyte* __real_glGetString(GLenum name);
const GLubyte* __wrap_glGetString(GLenum name) {
    if (name == GL_VERSION && gReportGles2) return (const GLubyte*) "OpenGL ES 2.0 (test_gl)";
    return __real_glGetString(name);
}

void __real_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void __wrap_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    gDrawCalls++;
    __real_glDrawArrays(mode, first, count);
}

void __real_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels);
void __wrap_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels) {
    gUploadedTexels += (long long) w * h;
    __real_glTexSubImage2D(target, level, x, y, w, h, format, type, pixels);
}

static void fatal(const char* what) {
    fprintf(stderr, "test_gl: %s (EGL error 0x%x, GL error 0x%x)\n", what, eglGetError(), glGetError());
    exit(2);
}

static void makeHeadlessContext(void) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (getPlatformDisplay == NULL) fatal("eglGetPlatformDisplayEXT unavailable");
    EGLDisplay display = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) {
        fatal("no surfaceless EGL display: this needs Mesa (see README.md)");
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
    EGLConfig config;
    EGLint configCount = 0;
    if (!eglChooseConfig(display, configAttribs, &config, 1, &configCount) || configCount == 0) fatal("no GLES 3 config");
    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
        fatal("cannot make a GLES 3 context current");
    }
}

static GLuint makeVideoFrame(int width, int height) {
    GLuint texture, fbo;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("incomplete framebuffer");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return fbo;
}

// x * y / 255, rounded, in integers.
static inline int mulUnorm8(int x, int y) {
    const int t = x * y + 0x80;
    return (t + (t >> 8)) >> 8;
}

// What the overlay's shader and premultiplied "over" blend compute, image by image in list order: the
// shader's output rounded to 8 bits, then blended in 8-bit fixed point.
static void compositeOnCpu(const ASS_Image* image, int width, int height, unsigned char* rgb) {
    for (size_t i = 0; i < (size_t) width * height; i++) memcpy(&rgb[i * 3], kBackground, 3);
    for (const ASS_Image* img = image; img != NULL; img = img->next) {
        const float color[3] = {(img->color >> 24) / 255.0f, ((img->color >> 16) & 0xFF) / 255.0f, ((img->color >> 8) & 0xFF) / 255.0f};
        const float opacity = (0xFF - (img->color & 0xFF)) / 255.0f;
        for (int y = 0; y < img->h; y++) {
            for (int x = 0; x < img->w; x++) {
                const int dstX = img->dst_x + x, dstY = img->dst_y + y;
                if (dstX < 0 || dstY < 0 || dstX >= width || dstY >= height) continue;
                const float a = opacity * (img->bitmap[(size_t) y * img->stride + x] / 255.0f);
                const int alpha = (int) (a * 255.0f + 0.5f);
                unsigned char* dst = &rgb[((size_t) dstY * width + dstX) * 3];
                for (int c = 0; c < 3; c++) {
                    const int blended = (int) (color[c] * a * 255.0f + 0.5f) + mulUnorm8(dst[c], 255 - alpha);
                    dst[c] = (unsigned char) (blended < 255 ? blended : 255);
                }
            }
        }
    }
}

int main(int argc, char** argv) {
    int maxFrames = -1;
    double fps = 23.976;
    for (int opt; (opt = getopt(argc, argv, "m:2n:r:")) != -1;) {
        switch (opt) {
        case 'm': gMaxTextureSize = atoi(optarg); break;
        case '2': gReportGles2 = true; break;
        case 'n': maxFrames = atoi(optarg); break;
        case 'r': fps = atof(optarg); break;
        default: optind = argc + 1; break;
        }
    }
    int width, height;
    if (optind + 3 != argc || sscanf(argv[optind + 2], "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0) {
        fprintf(stderr, "usage: %s [-m max-atlas-size] [-2] [-n frames] [-r fps] <track.ass> <fonts-dir> <width>x<height>\n", argv[0]);
        return 2;
    }
    const char* trackPath = argv[optind];

    makeHeadlessContext();
    const GLuint fbo = makeVideoFrame(width, height);
    GLint maxTextureSize;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize); // as the overlay will see it
    // nativeAssOverlayDraw sets the libass frame size itself, from the video frame's.
    LibassTrack t = openLibassTrack(trackPath, argv[optind + 1]);

    unsigned char* pixels = (unsigned char*) malloc((size_t) width * height * 4);
    unsigned char* expected = (unsigned char*) malloc((size_t) width * height * 3);
    if (pixels == NULL || expected == NULL) fatal("out of memory");
    jlong overlay = 0;
    uint64_t hash = 14695981039346656037ULL;
    long long uploadedTexels = 0;
    int frames = 0, framesWithSubtitles = 0, repacks = 0, maxPages = 0, maxDiff = 0, firstBadFrame = -1;
    for (long long frameIdx = 0; maxFrames < 0 || frameIdx < maxFrames; frameIdx++) {
        const long long timeMs = frameTimeMs(frameIdx, fps);
        if (timeMs > t.endMs) break;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glClearColor(kBackground[0] / 255.0f, kBackground[1] / 255.0f, kBackground[2] / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        gDrawCalls = 0;
        gUploadedTexels = 0;
        overlay = nativeAssOverlayDraw(NULL, NULL, overlay, (jlong) t.renderer, (jlong) t.track, (jint) fbo, width, height, timeMs);
        if (overlay == 0) fatal("nativeAssOverlayDraw failed");
        if (glGetError() != GL_NO_ERROR) fatal("GL error while drawing");
        if (gDrawCalls > 0) framesWithSubtitles++;
        if (gDrawCalls > maxPages) maxPages = gDrawCalls; // one draw call per page
        if (gUploadedTexels > 0) repacks++;
        uploadedTexels += gUploadedTexels;

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        for (size_t i = 0; i < (size_t) width * height * 4; i++) hash = (hash ^ pixels[i]) * 1099511628211ULL;

        // Asking again for the same time returns the images that were just drawn (changed == 0).
        int changed;
        compositeOnCpu(ass_render_frame(t.renderer, t.track, timeMs, &changed), width, height, expected);
        for (int y = 0; y < height; y++) {
            const unsigned char* row = &pixels[(size_t) (height - 1 - y) * width * 4]; // GL rows go bottom-up
            for (int x = 0; x < width; x++) {
                for (int c = 0; c < 3; c++) {
                    const int diff = abs((int) row[x * 4 + c] - (int) expected[((size_t) y * width + x) * 3 + c]);
                    if (diff > maxDiff) maxDiff = diff;
                    if (diff > MAX_CHANNEL_DIFF && firstBadFrame < 0) firstBadFrame = frames;
                }
            }
        }
        frames++;
    }

    printf("%s %dx%d, pages <= %d, %s: %d frames (%d with subtitles), %.2f MB uploaded per repack, up to %d pages, "
           "max diff vs CPU %d, hash %016llx\n",
        trackPath, width, height, maxTextureSize, gReportGles2 ? "GLES2" : "GLES3", frames, framesWithSubtitles,
        repacks > 0 ? uploadedTexels / 1e6 / repacks : 0.0, maxPages, maxDiff, (unsigned long long) hash);
    nativeAssOverlayRelease(NULL, NULL, overlay);
    closeLibassTrack(&t);
    free(pixels);
    free(expected);
    if (firstBadFrame >= 0) {
        fprintf(stderr, "FAIL %s: frame %d differs from the CPU compositing by more than %d\n", trackPath, firstBadFrame, MAX_CHANNEL_DIFF);
        return 1;
    }
    return 0;
}
