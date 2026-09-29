// ass_gl_overlay.cpp - libass -> Media3 effect chain, in-place GPU blending.
//
// Setup, after your existing libass init:
//   auto *c = new AssGl{renderer, track};
//   ass_set_storage_size(renderer, videoWidth, videoHeight);
//   pass reinterpret_cast<jlong>(c) to AssEffect(...)
// The frame size is set automatically from the frames the effect receives.
// If you feed events from another thread (ass_process_chunk, ...), hold c->mutex while doing it.
//
// Link with: GLESv2 ass log

#include <jni.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <ass/ass.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AssGl", __VA_ARGS__)
// Not part of the original shared reference file: added purely to profile this against
// AssOverlay.c's own timing logs (same tag convention, same threshold) for a fair A/B comparison.
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "AssGl", __VA_ARGS__)
#define AGL_SLOW_LOG_MS 2 // matches AssOverlay.c's ASS_OVERLAY_SLOW_LOG_MS

static inline long long aglNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Vertex {
    float x, y;          // frame pixels, origin top-left
    float u, v;          // atlas texels
    uint8_t r, g, b, a;  // premultiplied color
};

struct AssGl {
    ASS_Renderer *renderer;
    ASS_Track *track;
    std::mutex mutex;

    int frameW = 0, frameH = 0;
    GLuint program = 0, vbo = 0, atlas = 0;
    GLint aPos = -1, aUv = -1, aColor = -1;
    GLint uScale = -1, uAtlasInv = -1, uTex = -1;
    GLint maxTex = 0;
    int atlasW = 0, atlasH = 0;
    int vertexCount = 0;

    std::vector<uint8_t> staging;
    std::vector<int> slotX, slotY;  // atlas position of each ASS_Image, in list order
    std::vector<Vertex> verts;
};

static const char *kVertexShader = R"(
attribute vec2 a_pos;
attribute vec2 a_uv;
attribute vec4 a_color;
uniform vec2 u_scale;     // (2/W, -2/H): pixels -> NDC, image row 0 at the top
uniform vec2 u_atlasInv;  // (1/atlasW, 1/atlasH)
varying vec2 v_uv;
varying vec4 v_color;
void main() {
    v_uv = a_uv * u_atlasInv;
    v_color = a_color;
    gl_Position = vec4(a_pos * u_scale + vec2(-1.0, 1.0), 0.0, 1.0);
}
)";

static const char *kFragmentShader = R"(
precision mediump float;
uniform sampler2D u_tex;
varying highp vec2 v_uv;  // highp: fp16 can't address texels exactly in a large atlas
varying vec4 v_color;
void main() {
    gl_FragColor = v_color * texture2D(u_tex, v_uv).a;
}
)";

static GLuint compileShader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        LOGE("shader compile failed: %s", log);
    }
    return s;
}

static void initGl(AssGl &c) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    c.program = glCreateProgram();
    glAttachShader(c.program, vs);
    glAttachShader(c.program, fs);
    glLinkProgram(c.program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    c.aPos = glGetAttribLocation(c.program, "a_pos");
    c.aUv = glGetAttribLocation(c.program, "a_uv");
    c.aColor = glGetAttribLocation(c.program, "a_color");
    c.uScale = glGetUniformLocation(c.program, "u_scale");
    c.uAtlasInv = glGetUniformLocation(c.program, "u_atlasInv");
    c.uTex = glGetUniformLocation(c.program, "u_tex");

    glGenBuffers(1, &c.vbo);
    glGenTextures(1, &c.atlas);
    glBindTexture(GL_TEXTURE_2D, c.atlas);
    // Quads map 1:1 to frame pixels, so NEAREST is exact and needs no padding between slots.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &c.maxTex);
}

// Shelf-pack every bitmap into one 8-bit atlas and upload it with a single call.
static void uploadAtlas(AssGl &c, const ASS_Image *images) {
    const int W = std::min<int>(c.maxTex, std::max(2048, c.frameW));
    int x = 0, y = 0, shelfH = 0;
    c.slotX.clear();
    c.slotY.clear();
    for (const ASS_Image *i = images; i; i = i->next) {
        if (x + i->w > W) { x = 0; y += shelfH; shelfH = 0; }
        c.slotX.push_back(x);
        c.slotY.push_back(y);
        x += i->w;
        shelfH = std::max(shelfH, i->h);
    }
    const int usedH = y + shelfH;
    if (usedH == 0) return;
    // usedH > maxTex would need extreme typesetting (e.g. 4096x16384 texels of glyphs).
    // To be bulletproof, split into several atlases and draws; here it is only clamped.

    c.staging.resize(static_cast<size_t>(W) * usedH);
    size_t k = 0;
    for (const ASS_Image *i = images; i; i = i->next, ++k) {
        uint8_t *dst = c.staging.data() + static_cast<size_t>(c.slotY[k]) * W + c.slotX[k];
        for (int row = 0; row < i->h; ++row)
            memcpy(dst + static_cast<size_t>(row) * W, i->bitmap + static_cast<size_t>(row) * i->stride, i->w);
    }

    glBindTexture(GL_TEXTURE_2D, c.atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (W != c.atlasW || usedH > c.atlasH) {  // grow geometrically so reallocation is rare
        const int newH = (W == c.atlasW) ? std::max(usedH, c.atlasH * 2) : usedH;
        c.atlasW = W;
        c.atlasH = std::min<int>(newH, c.maxTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, c.atlasW, c.atlasH, 0, GL_ALPHA, GL_UNSIGNED_BYTE, nullptr);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, std::min(usedH, c.atlasH), GL_ALPHA, GL_UNSIGNED_BYTE,
                    c.staging.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// Two triangles per ASS_Image, color premultiplied. libass color is 0xRRGGBBAA, AA = transparency.
static void buildVertices(AssGl &c, const ASS_Image *images) {
    c.verts.clear();
    size_t k = 0;
    for (const ASS_Image *i = images; i && k < c.slotX.size(); i = i->next, ++k) {
        if (i->w == 0 || i->h == 0) continue;
        const uint32_t a = 255 - (i->color & 0xFF);
        const uint8_t r = static_cast<uint8_t>(((i->color >> 24) * a + 127) / 255);
        const uint8_t g = static_cast<uint8_t>((((i->color >> 16) & 0xFF) * a + 127) / 255);
        const uint8_t b = static_cast<uint8_t>((((i->color >> 8) & 0xFF) * a + 127) / 255);
        const uint8_t al = static_cast<uint8_t>(a);

        const float x0 = static_cast<float>(i->dst_x), y0 = static_cast<float>(i->dst_y);
        const float x1 = x0 + i->w, y1 = y0 + i->h;
        const float u0 = static_cast<float>(c.slotX[k]), v0 = static_cast<float>(c.slotY[k]);
        const float u1 = u0 + i->w, v1 = v0 + i->h;

        const Vertex quad[6] = {
            {x0, y0, u0, v0, r, g, b, al}, {x1, y0, u1, v0, r, g, b, al}, {x0, y1, u0, v1, r, g, b, al},
            {x1, y0, u1, v0, r, g, b, al}, {x1, y1, u1, v1, r, g, b, al}, {x0, y1, u0, v1, r, g, b, al},
        };
        c.verts.insert(c.verts.end(), quad, quad + 6);
    }
    c.vertexCount = static_cast<int>(c.verts.size());
    if (c.vertexCount > 0) {
        glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
        glBufferData(GL_ARRAY_BUFFER, c.verts.size() * sizeof(Vertex), c.verts.data(), GL_STREAM_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
}

static void drawSubtitles(AssGl &c, GLuint fbo, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);

    glUseProgram(c.program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, c.atlas);
    glUniform1i(c.uTex, 0);
    glUniform2f(c.uScale, 2.0f / w, -2.0f / h);
    glUniform2f(c.uAtlasInv, 1.0f / c.atlasW, 1.0f / c.atlasH);

    glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
    glEnableVertexAttribArray(c.aPos);
    glEnableVertexAttribArray(c.aUv);
    glEnableVertexAttribArray(c.aColor);
    glVertexAttribPointer(c.aPos, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void *>(offsetof(Vertex, x)));
    glVertexAttribPointer(c.aUv, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void *>(offsetof(Vertex, u)));
    glVertexAttribPointer(c.aColor, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                          reinterpret_cast<const void *>(offsetof(Vertex, r)));

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied "over"; video alpha stays 1
    glDrawArrays(GL_TRIANGLES, 0, c.vertexCount);

    // Leave GL state the way Media3's own programs expect it.
    glDisable(GL_BLEND);
    glDisableVertexAttribArray(c.aPos);
    glDisableVertexAttribArray(c.aUv);
    glDisableVertexAttribArray(c.aColor);
    glBindBuffer(GL_ARRAY_BUFFER, 0);  // a bound VBO would break client-side vertex arrays
    glBindTexture(GL_TEXTURE_2D, 0);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_subs_AssOverlayProgram_nativeDraw(JNIEnv *, jobject, jlong handle, jint fbo,
                                                   jint width, jint height, jlong ptsMs) {
    AssGl &c = *reinterpret_cast<AssGl *>(handle);
    std::lock_guard<std::mutex> lock(c.mutex);
    if (!c.program) initGl(c);

    bool sizeChanged = false;
    if (width != c.frameW || height != c.frameH) {
        c.frameW = width;
        c.frameH = height;
        ass_set_frame_size(c.renderer, width, height);
        sizeChanged = true;
        LOGW("ass_set_frame_size(%d, %d)", width, height);
    }

    const long long t0 = aglNowMs();
    int change = 0;
    ASS_Image *images = ass_render_frame(c.renderer, c.track, ptsMs, &change);
    const long long renderMs = aglNowMs() - t0;
    if (renderMs >= AGL_SLOW_LOG_MS) {
        LOGW("ass_render_frame(ptsMs=%lld) took %lldms (change=%d)", ptsMs, renderMs, change);
    }

    if (change == 2 || sizeChanged) {
        const long long t1 = aglNowMs();
        uploadAtlas(c, images);    // new bitmaps: repack + one upload
        const long long uploadMs = aglNowMs() - t1;
        if (uploadMs >= AGL_SLOW_LOG_MS) {
            LOGW("uploadAtlas(ptsMs=%lld) took %lldms", ptsMs, uploadMs);
        }

        const long long t2 = aglNowMs();
        buildVertices(c, images);
        const long long buildMs = aglNowMs() - t2;
        if (buildMs >= AGL_SLOW_LOG_MS) {
            LOGW("buildVertices(ptsMs=%lld) took %lldms", ptsMs, buildMs);
        }
    } else if (change == 1) {
        const long long t1 = aglNowMs();
        buildVertices(c, images);  // same bitmaps in the same order, only positions moved
        const long long buildMs = aglNowMs() - t1;
        if (buildMs >= AGL_SLOW_LOG_MS) {
            LOGW("buildVertices(ptsMs=%lld) took %lldms", ptsMs, buildMs);
        }
    }                              // change == 0: reuse atlas and VBO, just draw

    if (c.vertexCount > 0) {
        const long long t3 = aglNowMs();
        drawSubtitles(c, static_cast<GLuint>(fbo), width, height);
        const long long drawMs = aglNowMs() - t3;
        if (drawMs >= AGL_SLOW_LOG_MS) {
            LOGW("drawSubtitles(ptsMs=%lld) took %lldms", ptsMs, drawMs);
        }
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_subs_AssOverlayProgram_nativeReleaseGl(JNIEnv *, jobject, jlong handle) {
    AssGl &c = *reinterpret_cast<AssGl *>(handle);
    std::lock_guard<std::mutex> lock(c.mutex);
    if (c.program) glDeleteProgram(c.program);
    if (c.vbo) glDeleteBuffers(1, &c.vbo);
    if (c.atlas) glDeleteTextures(1, &c.atlas);
    c.program = c.vbo = c.atlas = 0;
    c.atlasW = c.atlasH = c.vertexCount = 0;
    c.frameW = c.frameH = 0;  // forces a full rebuild if a new GL context is used later
}

// ---- Everything below this line was added, not part of the shared reference file. ----
// The reference's own setup comment above ("auto *c = new AssGl{renderer, track}; ... pass
// reinterpret_cast<jlong>(c) to AssEffect(...)") is pseudocode, not a callable entry point - these
// two functions are the minimal, literal implementation of exactly that, so the paired Kotlin side
// (AssOverlayProgram's companion object) has something real to call.

extern "C" JNIEXPORT jlong JNICALL
Java_com_example_subs_AssOverlayProgram_nativeCreate(JNIEnv *, jclass, jlong rendererPtr, jlong trackPtr) {
    return reinterpret_cast<jlong>(new AssGl{reinterpret_cast<ASS_Renderer *>(rendererPtr),
                                              reinterpret_cast<ASS_Track *>(trackPtr)});
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_subs_AssOverlayProgram_nativeDestroy(JNIEnv *, jclass, jlong handle) {
    delete reinterpret_cast<AssGl *>(handle);
}
