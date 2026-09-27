#include "AssBlend.h"

#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ass/ass.h"
#include "GLES2/gl2.h"
#include "GLES2/gl2ext.h"

// ============================================================================
// EFFECTS_ATLAS: native-side video+subtitle blending for the GlEffect path.
//
// Renders subtitles into a single persistent GL texture atlas, then draws two
// passes directly into the caller-provided output framebuffer: a full-frame
// video blit, then a batched draw of all currently visible subtitle pieces.
//
// libass computation (ass_render_frame) runs on a dedicated worker thread,
// NOT on the GL thread that drawFrame is called on: a slow/complex subtitle
// frame must never stall video frame delivery past a bounded budget. The
// worker continuously packs results into one of two CPU-side "buffer sets";
// each drawFrame call hands the worker the latest requested time, then waits
// up to ctx->workerWaitMs (see nativeAssBlendConfigure /
// AssHandlerConfig.blendWorkerWaitMs) for the worker to finish that exact
// request before uploading/drawing whatever it last published. Real-world
// ass_render_frame cost varies a lot with content (profiling has shown
// anywhere from sub-millisecond to 10-15ms for moderately complex frames at a
// modest render size), so this is a per-app-configured budget rather than a
// fixed constant - tune it against your content's actual cost (logcat tag
// AssBlend) relative to your video's frame period: too low and frames that
// take longer than the budget get drawn one request behind every time (a
// `TIMED OUT` log line) even though the worker isn't falling behind overall;
// too high, especially on higher-frame-rate video, and a slow/complex frame
// can stall video delivery for up to that long.
//
// The worker thread calls back into Kotlin (AssRender.workerComputeBlendFrame)
// for each computation, rather than caching a raw ASS_Renderer*/ASS_Track*
// pointer across cycles: only the JVM side knows whether the track is still
// valid, and only by re-checking + locking on the JVM's `Ass.lock` right
// before each use (exactly like the existing synchronous renderFrame() path)
// can a stale pointer / use-after-free race with AssTrack.release() or track
// mutation (readChunk) be avoided.
// ============================================================================

#define ASS_BLEND_ATLAS_PADDING 2
#define ASS_BLEND_ATLAS_GROW_STEP 256
#define ASS_BLEND_ATLAS_MAX_SIZE 4096
#define ASS_BLEND_VERTEX_FLOATS 8 // x,y,u,v,r,g,b,a
#define ASS_BLEND_WORKER_WAIT_MS_MIN 1 // defensive floor if workerWaitMs is misconfigured to <=0
#define ASS_BLEND_LOG_TAG "AssBlend"
// Anything at or above this is logged as a candidate culprit for a missed/late subtitle frame;
// below it is normal jitter not worth the logcat noise.
#define ASS_BLEND_SLOW_LOG_MS 2

static inline long long assBlendNowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

// One packed subtitle piece, as published by the worker thread. Positions
// and color are carried raw (not yet clip-space/UV-normalized): that
// conversion depends on renderWidth/renderHeight and the atlas texture's
// current capacity, both GL-thread-owned state, so it happens at consume
// time on the GL thread instead of being baked in by the worker.
typedef struct {
    int atlasX, atlasY;
    int dstX, dstY, w, h;
    float r, g, b, a;
} AssBlendPiece;

// A complete packed-atlas result: one contiguous CPU pixel buffer plus the
// per-piece placement/metadata needed to build vertices from it. The worker
// and the GL thread each own one of these at a time (see AssBlendContext
// below) and trade ownership via a pointer swap under `mutex`.
typedef struct {
    unsigned char* atlasBuf;
    int atlasCap;
    int atlasW, atlasH; // tight packed size actually used
    AssBlendPiece* pieces;
    int pieceCap;
    int pieceCount;
} AssBlendBufferSet;

typedef struct {
    int idx;
    int w, h;
} AssBlendSortItem;

typedef struct {
    // ---- GL-thread-only state (never touched by the worker thread) ----
    int maxAtlasSize;
    int renderWidth;
    int renderHeight;
    int workerWaitMs; // see nativeAssBlendConfigure / AssHandlerConfig.blendWorkerWaitMs

    GLuint atlasTex;
    int atlasCapW;
    int atlasCapH;

    GLuint blitProgram;
    GLint blitAPosition;
    GLint blitUTexture;
    GLuint blitVbo;

    GLuint atlasProgram;
    GLint atlasAPosition;
    GLint atlasATexCoord;
    GLint atlasAColor;
    GLint atlasUTexture;
    GLuint atlasVbo;

    float* vertexBuf; // interleaved atlas vertex data, ASS_BLEND_VERTEX_FLOATS per vertex
    int vertexCapFloats;
    int lastPlacedCount;      // pieces currently uploaded to atlasVbo/atlasTex, drawn every frame
    int consumedResultVersion; // last resultVersion this thread has uploaded

    // ---- worker-thread-only state (never touched by the GL thread) ----
    ASS_Image** scratchImages;
    AssBlendSortItem* scratchItems;
    int* scratchPlaceX;
    int* scratchPlaceY;
    int scratchCap;
    int workerConsumedVersion; // last requestVersion the worker has started computing

    // ---- worker lifecycle ----
    JavaVM* jvm;
    jobject renderGlobalRef;       // global ref to the owning Kotlin AssRender instance
    jmethodID workerComputeMethodId; // AssRender.workerComputeBlendFrame(J)Z
    pthread_t workerThread;
    int workerStarted;

    // ---- cross-thread mailbox, guarded by `mutex` ----
    pthread_mutex_t mutex;
    pthread_cond_t cond;     // GL thread -> worker: new request available
    pthread_cond_t doneCond; // worker -> GL thread: workerDoneVersion advanced
    int shouldStop;
    long long requestedTimeMs;
    int requestVersion;
    int workerDoneVersion; // last requestVersion the worker has *finished* processing

    int resultVersion;
    AssBlendBufferSet bufA;
    AssBlendBufferSet bufB;
    AssBlendBufferSet* pendingSet; // published: GL thread reads this
    AssBlendBufferSet* workerSet;  // being filled by the worker
} AssBlendContext;

static GLuint assBlendCompileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        __android_log_print(ANDROID_LOG_ERROR, ASS_BLEND_LOG_TAG, "AssBlend shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint assBlendLinkProgram(const char* vsSrc, const char* fsSrc) {
    GLuint vs = assBlendCompileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = assBlendCompileShader(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (status != GL_TRUE) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        __android_log_print(ANDROID_LOG_ERROR, ASS_BLEND_LOG_TAG, "AssBlend program link error: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

// Samples the input video texture unchanged. Texcoord derivation
// (position * 0.5 + 0.5, no flip) mirrors media3's own
// vertex_shader_transformation_es2.glsl so inputTexId is sampled with the
// same convention media3 already established for it.
static const char* kAssBlendBlitVertexShader =
    "attribute vec4 a_Position;\n"
    "varying vec2 v_TexCoord;\n"
    "void main() {\n"
    "    gl_Position = a_Position;\n"
    "    v_TexCoord = vec2(a_Position.x * 0.5 + 0.5, a_Position.y * 0.5 + 0.5);\n"
    "}\n";

static const char* kAssBlendBlitFragmentShader =
    "precision mediump float;\n"
    "varying vec2 v_TexCoord;\n"
    "uniform sampler2D u_Texture;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_Texture, v_TexCoord);\n"
    "}\n";

// Draws every currently-visible subtitle piece in one batched call. Colors
// vary per piece so they travel as a per-vertex attribute (not a uniform,
// since one draw call now spans many differently-colored pieces). Output is
// fully premultiplied (by both opacity and glyph coverage) so it can be
// composited with a standard premultiplied "over" blend function.
static const char* kAssBlendAtlasVertexShader =
    "attribute vec2 a_Position;\n"
    "attribute vec2 a_TexCoord;\n"
    "attribute vec4 a_Color;\n"
    "varying vec2 v_TexCoord;\n"
    "varying vec4 v_Color;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_Position, 0.0, 1.0);\n"
    "    v_TexCoord = a_TexCoord;\n"
    "    v_Color = a_Color;\n"
    "}\n";

static const char* kAssBlendAtlasFragmentShader =
    "precision mediump float;\n"
    "varying vec2 v_TexCoord;\n"
    "varying vec4 v_Color;\n"
    "uniform sampler2D u_Texture;\n"
    "void main() {\n"
    "    float coverage = texture2D(u_Texture, v_TexCoord).a;\n"
    "    float a = v_Color.a * coverage;\n"
    "    gl_FragColor = vec4(v_Color.rgb * a, a);\n"
    "}\n";

static int assBlendCompareHeightDesc(const void* a, const void* b) {
    return ((const AssBlendSortItem*) b)->h - ((const AssBlendSortItem*) a)->h;
}

// Grows the worker's persistent scratch arrays (used only for packing math,
// never shared with the GL thread) to hold at least `count` entries. Leaves
// ctx->scratchCap unchanged (smaller than `count`) if any allocation fails,
// which callers must check before indexing up to `count`.
static void assBlendCtxEnsureScratch(AssBlendContext* ctx, int count) {
    if (count <= ctx->scratchCap) return;
    ASS_Image** images = (ASS_Image**) realloc(ctx->scratchImages, (size_t) count * sizeof(ASS_Image*));
    if (images) ctx->scratchImages = images;
    AssBlendSortItem* items = (AssBlendSortItem*) realloc(ctx->scratchItems, (size_t) count * sizeof(AssBlendSortItem));
    if (items) ctx->scratchItems = items;
    int* placeX = (int*) realloc(ctx->scratchPlaceX, (size_t) count * sizeof(int));
    if (placeX) ctx->scratchPlaceX = placeX;
    int* placeY = (int*) realloc(ctx->scratchPlaceY, (size_t) count * sizeof(int));
    if (placeY) ctx->scratchPlaceY = placeY;
    if (images && items && placeX && placeY) {
        ctx->scratchCap = count;
    }
}

static void assBlendBufferSetEnsureCapacity(AssBlendBufferSet* set, int atlasBytesNeeded, int pieceCountNeeded) {
    if (atlasBytesNeeded > set->atlasCap) {
        unsigned char* grown = (unsigned char*) realloc(set->atlasBuf, (size_t) atlasBytesNeeded);
        if (grown) {
            set->atlasBuf = grown;
            set->atlasCap = atlasBytesNeeded;
        }
    }
    if (pieceCountNeeded > set->pieceCap) {
        AssBlendPiece* grown = (AssBlendPiece*) realloc(set->pieces, (size_t) pieceCountNeeded * sizeof(AssBlendPiece));
        if (grown) {
            set->pieces = grown;
            set->pieceCap = pieceCountNeeded;
        }
    }
}

// Same tally AssKt.c's nativeAssRenderFrame does (count_ass_images there); kept as a separate
// static copy here rather than shared across translation units for such a trivial helper.
static int assBlendCountImages(ASS_Image* images) {
    int count = 0;
    for (ASS_Image* img = images; img != NULL; img = img->next) {
        count++;
    }
    return count;
}

// Runs on the worker thread only: shelf-packs `image`'s pieces and memcpy's
// their (stride-corrected) pixels into ctx->workerSet's staging buffer. Pure
// CPU work, no GL calls (the worker has no EGL context) — GL upload happens
// later, on the GL thread, from whatever this ends up publishing. Placement
// is computed in height-descending order for packing efficiency, but pieces
// are written into the pieces[] array in their ORIGINAL libass list order:
// overlapping same-position border/fill layers must stay in that order for
// the GL thread's "over" blending to composite them correctly.
static void assBlendWorkerPack(AssBlendContext* ctx, ASS_Image* image) {
    AssBlendBufferSet* set = ctx->workerSet;
    set->pieceCount = 0;
    set->atlasW = 0;
    set->atlasH = 0;

    int count = assBlendCountImages(image);
    if (count <= 0) return;

    assBlendCtxEnsureScratch(ctx, count);
    if (ctx->scratchCap < count) return; // OOM: skip this cycle, previous published result stays visible

    int i = 0;
    for (ASS_Image* img = image; img != NULL; img = img->next, i++) {
        ctx->scratchImages[i] = img;
        ctx->scratchItems[i].idx = i;
        ctx->scratchItems[i].w = img->w;
        ctx->scratchItems[i].h = img->h;
    }

    qsort(ctx->scratchItems, count, sizeof(AssBlendSortItem), assBlendCompareHeightDesc);

    int maxSize = ctx->maxAtlasSize;
    int shelfX = 0, shelfY = 0, shelfH = 0;
    int usedW = 0, usedH = 0;
    for (i = 0; i < count; i++) {
        int idx = ctx->scratchItems[i].idx;
        int w = ctx->scratchItems[i].w;
        int h = ctx->scratchItems[i].h;
        if (w <= 0 || h <= 0) {
            ctx->scratchPlaceX[idx] = -1;
            ctx->scratchPlaceY[idx] = -1;
            continue;
        }
        int pw = w + ASS_BLEND_ATLAS_PADDING;
        int ph = h + ASS_BLEND_ATLAS_PADDING;
        if (pw > maxSize) {
            // Single piece wider than the whole atlas cap: cannot place (extreme edge case).
            ctx->scratchPlaceX[idx] = -1;
            ctx->scratchPlaceY[idx] = -1;
            continue;
        }
        if (shelfX + pw > maxSize) {
            shelfY += shelfH;
            shelfX = 0;
            shelfH = 0;
        }
        if (shelfY + ph > maxSize) {
            // Atlas is full: drop this piece for this frame instead of overflowing (graceful degrade).
            ctx->scratchPlaceX[idx] = -1;
            ctx->scratchPlaceY[idx] = -1;
            continue;
        }
        ctx->scratchPlaceX[idx] = shelfX;
        ctx->scratchPlaceY[idx] = shelfY;
        shelfX += pw;
        if (ph > shelfH) shelfH = ph;
        if (shelfX > usedW) usedW = shelfX;
        if (shelfY + shelfH > usedH) usedH = shelfY + shelfH;
    }

    if (usedW <= 0 || usedH <= 0) return; // nothing placeable this frame; published result stays "0 pieces"

    assBlendBufferSetEnsureCapacity(set, usedW * usedH, count);
    if (set->atlasCap < usedW * usedH || set->pieceCap < count) return; // OOM: skip this cycle

    int placedCount = 0;
    for (i = 0; i < count; i++) {
        if (ctx->scratchPlaceX[i] < 0) continue;
        ASS_Image* img = ctx->scratchImages[i];
        int ax = ctx->scratchPlaceX[i];
        int ay = ctx->scratchPlaceY[i];
        for (int row = 0; row < img->h; row++) {
            memcpy(set->atlasBuf + (size_t) (ay + row) * usedW + ax, img->bitmap + (size_t) row * img->stride, (size_t) img->w);
        }
        AssBlendPiece* p = &set->pieces[placedCount];
        p->atlasX = ax;
        p->atlasY = ay;
        p->dstX = img->dst_x;
        p->dstY = img->dst_y;
        p->w = img->w;
        p->h = img->h;
        p->r = ((img->color >> 24) & 0xFF) / 255.0f;
        p->g = ((img->color >> 16) & 0xFF) / 255.0f;
        p->b = ((img->color >> 8) & 0xFF) / 255.0f;
        p->a = (0xFF - (img->color & 0xFF)) / 255.0f;
        placedCount++;
    }

    set->atlasW = usedW;
    set->atlasH = usedH;
    set->pieceCount = placedCount;
}

// Hands the just-filled ctx->workerSet to the GL thread by swapping it with
// ctx->pendingSet (O(1), no copy). The GL thread only ever reads whichever
// set is currently `pendingSet` while holding `mutex`, so this swap is safe
// regardless of how far behind the GL thread's consumption is — an
// unconsumed previous result is simply superseded (latest-wins), never read
// concurrently with the worker overwriting it.
static void assBlendPublish(AssBlendContext* ctx) {
    pthread_mutex_lock(&ctx->mutex);
    AssBlendBufferSet* tmp = ctx->pendingSet;
    ctx->pendingSet = ctx->workerSet;
    ctx->workerSet = tmp;
    ctx->resultVersion++;
    pthread_mutex_unlock(&ctx->mutex);
}

// The worker thread's main loop. Attaches to the JVM once (raw ASS_Renderer*/
// ASS_Track* pointers are never cached across cycles — see the file header
// comment — so every cycle instead calls back into Kotlin, which re-validates
// and locks before touching native state). Waits for a new requested time,
// computes, publishes, repeats; exits when nativeAssBlendRelease signals it to stop.
static void* assBlendWorkerMain(void* arg) {
    AssBlendContext* ctx = (AssBlendContext*) arg;
    JNIEnv* workerEnv = NULL;
    if ((*ctx->jvm)->AttachCurrentThread(ctx->jvm, &workerEnv, NULL) != JNI_OK) {
        return NULL;
    }

    pthread_mutex_lock(&ctx->mutex);
    for (;;) {
        while (!ctx->shouldStop && ctx->requestVersion == ctx->workerConsumedVersion) {
            pthread_cond_wait(&ctx->cond, &ctx->mutex);
        }
        if (ctx->shouldStop) {
            pthread_mutex_unlock(&ctx->mutex);
            break;
        }
        jlong timeMs = (jlong) ctx->requestedTimeMs;
        int consumedVersion = ctx->requestVersion;
        ctx->workerConsumedVersion = consumedVersion;
        pthread_mutex_unlock(&ctx->mutex);

        // Covers the whole JNI round trip: Kotlin's Ass.lock (contends with readChunk/track
        // mutation on the playback thread) plus nativeAssBlendWorkerCompute's own ass_render_frame
        // timing (logged separately, tag AssBlend). A gap between the two points at lock
        // contention rather than libass itself being slow.
        long long callT0 = assBlendNowMs();
        (*workerEnv)->CallBooleanMethod(workerEnv, ctx->renderGlobalRef, ctx->workerComputeMethodId, timeMs);
        long long callMs = assBlendNowMs() - callT0;
        if ((*workerEnv)->ExceptionCheck(workerEnv)) {
            (*workerEnv)->ExceptionClear(workerEnv);
        }
        if (callMs >= ASS_BLEND_SLOW_LOG_MS) {
            __android_log_print(ANDROID_LOG_WARN, ASS_BLEND_LOG_TAG,
                "workerComputeBlendFrame JNI round trip(timeMs=%lld) took %lldms total", timeMs, callMs);
        }

        pthread_mutex_lock(&ctx->mutex);
        // Mark this request fully handled (regardless of whether it changed/published anything) and
        // wake any drawFrame call bounded-waiting on it. Cheap: same lock nativeAssBlendDrawFrame
        // already takes to swap in the requested time.
        ctx->workerDoneVersion = consumedVersion;
        pthread_cond_broadcast(&ctx->doneCond);
    }

    (*ctx->jvm)->DetachCurrentThread(ctx->jvm);
    return NULL;
}

jlong nativeAssBlendConfigure(JNIEnv* env, jclass clazz, jlong blend, jlong render, jint renderWidth, jint renderHeight, jint workerWaitMs, jobject renderObj) {
    if (!render) return 0;
    AssBlendContext* ctx = (AssBlendContext*) blend;
    if (ctx == NULL) {
        ctx = (AssBlendContext*) calloc(1, sizeof(AssBlendContext));
        if (ctx == NULL) return 0;

        GLint maxTexSize = ASS_BLEND_ATLAS_MAX_SIZE;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexSize);
        ctx->maxAtlasSize = maxTexSize < ASS_BLEND_ATLAS_MAX_SIZE ? maxTexSize : ASS_BLEND_ATLAS_MAX_SIZE;

        ctx->blitProgram = assBlendLinkProgram(kAssBlendBlitVertexShader, kAssBlendBlitFragmentShader);
        ctx->blitAPosition = glGetAttribLocation(ctx->blitProgram, "a_Position");
        ctx->blitUTexture = glGetUniformLocation(ctx->blitProgram, "u_Texture");

        ctx->atlasProgram = assBlendLinkProgram(kAssBlendAtlasVertexShader, kAssBlendAtlasFragmentShader);
        ctx->atlasAPosition = glGetAttribLocation(ctx->atlasProgram, "a_Position");
        ctx->atlasATexCoord = glGetAttribLocation(ctx->atlasProgram, "a_TexCoord");
        ctx->atlasAColor = glGetAttribLocation(ctx->atlasProgram, "a_Color");
        ctx->atlasUTexture = glGetUniformLocation(ctx->atlasProgram, "u_Texture");

        GLuint buffers[2];
        glGenBuffers(2, buffers);
        ctx->blitVbo = buffers[0];
        ctx->atlasVbo = buffers[1];

        static const GLfloat blitVerts[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
        glBindBuffer(GL_ARRAY_BUFFER, ctx->blitVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(blitVerts), blitVerts, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        pthread_mutex_init(&ctx->mutex, NULL);
        pthread_cond_init(&ctx->cond, NULL);
        // CLOCK_MONOTONIC explicitly: nativeAssBlendDrawFrame's bounded wait computes its deadline
        // from CLOCK_MONOTONIC too, so this must match regardless of the platform's default
        // pthread_cond_timedwait clock, and is immune to wall-clock (RTC) adjustments mid-wait.
        pthread_condattr_t doneCondAttr;
        pthread_condattr_init(&doneCondAttr);
        pthread_condattr_setclock(&doneCondAttr, CLOCK_MONOTONIC);
        pthread_cond_init(&ctx->doneCond, &doneCondAttr);
        pthread_condattr_destroy(&doneCondAttr);
        ctx->pendingSet = &ctx->bufA;
        ctx->workerSet = &ctx->bufB;

        (*env)->GetJavaVM(env, &ctx->jvm);
        ctx->renderGlobalRef = (*env)->NewGlobalRef(env, renderObj);
        jclass renderClass = (*env)->GetObjectClass(env, renderObj);
        ctx->workerComputeMethodId = renderClass != NULL
            ? (*env)->GetMethodID(env, renderClass, "workerComputeBlendFrame", "(J)Z")
            : NULL;

        if (ctx->renderGlobalRef != NULL && ctx->workerComputeMethodId != NULL) {
            if (pthread_create(&ctx->workerThread, NULL, assBlendWorkerMain, ctx) == 0) {
                ctx->workerStarted = 1;
            }
        }
    }
    ctx->renderWidth = renderWidth;
    ctx->renderHeight = renderHeight;
    ctx->workerWaitMs = workerWaitMs > ASS_BLEND_WORKER_WAIT_MS_MIN ? workerWaitMs : ASS_BLEND_WORKER_WAIT_MS_MIN;
    ass_set_frame_size((ASS_Renderer*) render, renderWidth, renderHeight);
    __android_log_print(ANDROID_LOG_INFO, ASS_BLEND_LOG_TAG,
        "ass_set_frame_size(%d, %d), workerWaitMs=%d", renderWidth, renderHeight, ctx->workerWaitMs);
    return (jlong) ctx;
}

// Called back from the worker thread (via Kotlin's AssRender.workerComputeBlendFrame, which holds
// the JVM-side Ass.lock for the duration of this call — see the file header comment). Never called
// from the GL thread.
jboolean nativeAssBlendWorkerCompute(JNIEnv* env, jclass clazz, jlong blend, jlong render, jlong track, jlong timeMs) {
    if (!blend || !render || !track) return JNI_FALSE;
    AssBlendContext* ctx = (AssBlendContext*) blend;

    long long t0 = assBlendNowMs();
    int changed = 0;
    ASS_Image* image = ass_render_frame((ASS_Renderer*) render, (ASS_Track*) track, timeMs, &changed);
    long long renderMs = assBlendNowMs() - t0;
    if (renderMs >= ASS_BLEND_SLOW_LOG_MS) {
        __android_log_print(ANDROID_LOG_WARN, ASS_BLEND_LOG_TAG,
            "ass_render_frame(timeMs=%lld) took %lldms (changed=%d)", timeMs, renderMs, changed);
    }
    if (changed) {
        long long t1 = assBlendNowMs();
        assBlendWorkerPack(ctx, image);
        long long packMs = assBlendNowMs() - t1;
        if (packMs >= ASS_BLEND_SLOW_LOG_MS) {
            __android_log_print(ANDROID_LOG_WARN, ASS_BLEND_LOG_TAG,
                "assBlendWorkerPack(timeMs=%lld) took %lldms", timeMs, packMs);
        }

        long long t2 = assBlendNowMs();
        assBlendPublish(ctx);
        long long publishMs = assBlendNowMs() - t2;
        if (publishMs >= ASS_BLEND_SLOW_LOG_MS) {
            __android_log_print(ANDROID_LOG_WARN, ASS_BLEND_LOG_TAG,
                "assBlendPublish(timeMs=%lld) took %lldms", timeMs, publishMs);
        }
    }
    return JNI_TRUE;
}

// Called every video frame, on the GL thread. Never calls ass_render_frame itself: hands the
// worker the latest requested time, then bounded-waits (up to ctx->workerWaitMs) for the worker to
// finish that exact request before uploading+drawing whatever it last published. See the file
// header comment for why this is bounded rather than either fully blocking or not waiting at all.
void nativeAssBlendDrawFrame(JNIEnv* env, jclass clazz, jlong blend, jlong timeMs, jint inputTexId) {
    if (!blend) return;
    AssBlendContext* ctx = (AssBlendContext*) blend;
    if (ctx->blitProgram == 0 || ctx->atlasProgram == 0) return;

    pthread_mutex_lock(&ctx->mutex);
    ctx->requestedTimeMs = timeMs;
    ctx->requestVersion++;
    int myRequest = ctx->requestVersion;
    pthread_cond_signal(&ctx->cond);

    // If the worker thread never started (see nativeAssBlendConfigure), workerDoneVersion can never
    // advance — skip the wait entirely instead of eating the full timeout on every frame.
    if (ctx->workerStarted) {
        long long waitT0 = assBlendNowMs();
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_nsec += (long) ctx->workerWaitMs * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += deadline.tv_nsec / 1000000000L;
            deadline.tv_nsec %= 1000000000L;
        }
        int timedOut = 0;
        while (ctx->workerDoneVersion < myRequest) {
            if (pthread_cond_timedwait(&ctx->doneCond, &ctx->mutex, &deadline) == ETIMEDOUT) {
                timedOut = (ctx->workerDoneVersion < myRequest);
                break;
            }
        }
        long long waitedMs = assBlendNowMs() - waitT0;
        if (timedOut) {
            // The single clearest "this is why a frame was wrong" signal: the draw call gave up
            // and is about to use whatever atlas the worker last published, which lags this
            // request (myRequest) by (myRequest - workerDoneVersion) requests.
            __android_log_print(ANDROID_LOG_ERROR, ASS_BLEND_LOG_TAG,
                "draw(timeMs=%lld) TIMED OUT after %lldms waiting for worker (myRequest=%d, workerDoneVersion=%d, behind by %d) — drawing stale atlas",
                timeMs, waitedMs, myRequest, ctx->workerDoneVersion, myRequest - ctx->workerDoneVersion);
        } else if (waitedMs >= ASS_BLEND_SLOW_LOG_MS) {
            __android_log_print(ANDROID_LOG_WARN, ASS_BLEND_LOG_TAG,
                "draw(timeMs=%lld) waited %lldms for worker to catch up (myRequest=%d)", timeMs, waitedMs, myRequest);
        }
    }

    if (ctx->resultVersion != ctx->consumedResultVersion) {
        ctx->consumedResultVersion = ctx->resultVersion;
        AssBlendBufferSet* pending = ctx->pendingSet;

        if (pending->pieceCount > 0 && pending->atlasW > 0 && pending->atlasH > 0) {
            if (pending->atlasW > ctx->atlasCapW || pending->atlasH > ctx->atlasCapH) {
                int newW = ctx->atlasCapW > pending->atlasW ? ctx->atlasCapW : pending->atlasW;
                int newH = ctx->atlasCapH > pending->atlasH ? ctx->atlasCapH : pending->atlasH;
                newW = ((newW + ASS_BLEND_ATLAS_GROW_STEP - 1) / ASS_BLEND_ATLAS_GROW_STEP) * ASS_BLEND_ATLAS_GROW_STEP;
                newH = ((newH + ASS_BLEND_ATLAS_GROW_STEP - 1) / ASS_BLEND_ATLAS_GROW_STEP) * ASS_BLEND_ATLAS_GROW_STEP;
                if (newW > ctx->maxAtlasSize) newW = ctx->maxAtlasSize;
                if (newH > ctx->maxAtlasSize) newH = ctx->maxAtlasSize;
                if (ctx->atlasTex) glDeleteTextures(1, &ctx->atlasTex);
                glGenTextures(1, &ctx->atlasTex);
                glBindTexture(GL_TEXTURE_2D, ctx->atlasTex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                // GL_LINEAR, not GL_NEAREST: whenever AssHandlerConfig.maxRenderPixels downscales
                // rendering (see computeRenderSize), this atlas is drawn magnified back up to the
                // full output size (the vertex positions below are already scaled for that), and
                // GL_NEAREST under magnification looks blocky/pixelated. The per-piece UV rect below
                // is inset by half a texel on every edge specifically so this doesn't sample past
                // each piece's own bitmap into ASS_BLEND_ATLAS_PADDING's (uninitialized) padding or a
                // neighboring packed piece.
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, newW, newH, 0, GL_ALPHA, GL_UNSIGNED_BYTE, NULL);
                ctx->atlasCapW = newW;
                ctx->atlasCapH = newH;
            } else {
                glBindTexture(GL_TEXTURE_2D, ctx->atlasTex);
            }
            // The worker's staging buffer is tightly packed (stride == atlasW), so this is one
            // plain upload — no GL_UNPACK_ROW_LENGTH_EXT / stride tricks needed on this side.
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pending->atlasW, pending->atlasH, GL_ALPHA, GL_UNSIGNED_BYTE, pending->atlasBuf);

            int neededFloats = pending->pieceCount * 6 * ASS_BLEND_VERTEX_FLOATS;
            if (neededFloats > ctx->vertexCapFloats) {
                float* grown = (float*) realloc(ctx->vertexBuf, (size_t) neededFloats * sizeof(float));
                if (grown) {
                    ctx->vertexBuf = grown;
                    ctx->vertexCapFloats = neededFloats;
                }
            }

            if (ctx->vertexBuf != NULL && ctx->vertexCapFloats >= neededFloats && ctx->renderWidth > 0 && ctx->renderHeight > 0) {
                int vi = 0;
                for (int i = 0; i < pending->pieceCount; i++) {
                    AssBlendPiece* p = &pending->pieces[i];
                    float x0 = (float) p->dstX;
                    float y0 = (float) p->dstY;
                    float x1 = x0 + (float) p->w;
                    float y1 = y0 + (float) p->h;
                    // Clip-space Y-flip: reproduces what a per-piece glViewport trick would achieve
                    // implicitly (libass y is top-down, GL clip y is bottom-up).
                    float cx0 = (x0 / (float) ctx->renderWidth) * 2.0f - 1.0f;
                    float cx1 = (x1 / (float) ctx->renderWidth) * 2.0f - 1.0f;
                    float cy0 = 1.0f - (y0 / (float) ctx->renderHeight) * 2.0f;
                    float cy1 = 1.0f - (y1 / (float) ctx->renderHeight) * 2.0f;

                    // Half-texel inset on every edge: with GL_LINEAR, keeps sampling strictly inside
                    // this piece's own bitmap even at the quad's outermost edge, so magnification
                    // never blends in ASS_BLEND_ATLAS_PADDING's padding or a neighboring piece.
                    float u0 = ((float) p->atlasX + 0.5f) / (float) ctx->atlasCapW;
                    float u1 = ((float) (p->atlasX + p->w) - 0.5f) / (float) ctx->atlasCapW;
                    float v0 = ((float) p->atlasY + 0.5f) / (float) ctx->atlasCapH;
                    float v1 = ((float) (p->atlasY + p->h) - 0.5f) / (float) ctx->atlasCapH;

                    // Two triangles, matching the same paint (list) order the worker packed them in.
                    float verts[6][4] = {
                        {cx0, cy0, u0, v0}, {cx1, cy0, u1, v0}, {cx0, cy1, u0, v1},
                        {cx1, cy0, u1, v0}, {cx1, cy1, u1, v1}, {cx0, cy1, u0, v1},
                    };
                    float* out = ctx->vertexBuf + vi;
                    for (int vtx = 0; vtx < 6; vtx++) {
                        *out++ = verts[vtx][0];
                        *out++ = verts[vtx][1];
                        *out++ = verts[vtx][2];
                        *out++ = verts[vtx][3];
                        *out++ = p->r;
                        *out++ = p->g;
                        *out++ = p->b;
                        *out++ = p->a;
                    }
                    vi += 6 * ASS_BLEND_VERTEX_FLOATS;
                }
                glBindBuffer(GL_ARRAY_BUFFER, ctx->atlasVbo);
                // Buffer orphaning (full glBufferData respecification) is the standard pattern for
                // per-frame-changing vertex streams; avoids read-after-write hazards vs. glBufferSubData.
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) vi * sizeof(float), ctx->vertexBuf, GL_DYNAMIC_DRAW);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                ctx->lastPlacedCount = pending->pieceCount;
            } else {
                ctx->lastPlacedCount = 0;
            }
        } else {
            ctx->lastPlacedCount = 0;
        }
    }
    pthread_mutex_unlock(&ctx->mutex);

    GLint prevProgram = 0, prevArrayBuffer = 0, prevTexture = 0;
    GLboolean prevBlendEnabled = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuffer);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);

    // Pass 1: video blit. Output FBO is already bound+cleared by the caller (BaseGlShaderProgram),
    // so this alone reproduces the current frame regardless of whether subtitle content changed.
    glDisable(GL_BLEND);
    glUseProgram(ctx->blitProgram);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->blitVbo);
    glEnableVertexAttribArray((GLuint) ctx->blitAPosition);
    glVertexAttribPointer((GLuint) ctx->blitAPosition, 2, GL_FLOAT, GL_FALSE, 0, (void*) 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint) inputTexId);
    glUniform1i(ctx->blitUTexture, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray((GLuint) ctx->blitAPosition);

    // Pass 2: batched subtitle overlay, one draw call for every currently visible piece.
    if (ctx->lastPlacedCount > 0 && ctx->atlasTex != 0) {
        glEnable(GL_BLEND);
        // Fragment shader outputs fully premultiplied color (opacity * coverage), so blend with a
        // standard premultiplied "over": GL_ONE (not GL_SRC_ALPHA) for the source factor.
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(ctx->atlasProgram);
        glBindBuffer(GL_ARRAY_BUFFER, ctx->atlasVbo);
        const GLsizei stride = ASS_BLEND_VERTEX_FLOATS * sizeof(GLfloat);
        glEnableVertexAttribArray((GLuint) ctx->atlasAPosition);
        glVertexAttribPointer((GLuint) ctx->atlasAPosition, 2, GL_FLOAT, GL_FALSE, stride, (void*) 0);
        glEnableVertexAttribArray((GLuint) ctx->atlasATexCoord);
        glVertexAttribPointer((GLuint) ctx->atlasATexCoord, 2, GL_FLOAT, GL_FALSE, stride, (void*) (2 * sizeof(GLfloat)));
        glEnableVertexAttribArray((GLuint) ctx->atlasAColor);
        glVertexAttribPointer((GLuint) ctx->atlasAColor, 4, GL_FLOAT, GL_FALSE, stride, (void*) (4 * sizeof(GLfloat)));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ctx->atlasTex);
        glUniform1i(ctx->atlasUTexture, 0);
        glDrawArrays(GL_TRIANGLES, 0, ctx->lastPlacedCount * 6);
        glDisableVertexAttribArray((GLuint) ctx->atlasAPosition);
        glDisableVertexAttribArray((GLuint) ctx->atlasATexCoord);
        glDisableVertexAttribArray((GLuint) ctx->atlasAColor);
    }

    glBindBuffer(GL_ARRAY_BUFFER, (GLuint) prevArrayBuffer);
    glBindTexture(GL_TEXTURE_2D, (GLuint) prevTexture);
    glUseProgram((GLuint) prevProgram);
    if (prevBlendEnabled) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
}

void nativeAssBlendRelease(JNIEnv* env, jclass clazz, jlong blend) {
    if (!blend) return;
    AssBlendContext* ctx = (AssBlendContext*) blend;

    if (ctx->workerStarted) {
        pthread_mutex_lock(&ctx->mutex);
        ctx->shouldStop = 1;
        pthread_cond_signal(&ctx->cond);
        pthread_mutex_unlock(&ctx->mutex);
        // May block briefly for the worker's current in-flight computation (and the JVM lock it
        // needs to finish it) to complete — acceptable here, this is a one-time teardown call, not
        // part of the per-frame path this whole design exists to keep non-blocking.
        pthread_join(ctx->workerThread, NULL);
    }
    if (ctx->renderGlobalRef != NULL) {
        (*env)->DeleteGlobalRef(env, ctx->renderGlobalRef);
    }
    pthread_mutex_destroy(&ctx->mutex);
    pthread_cond_destroy(&ctx->cond);
    pthread_cond_destroy(&ctx->doneCond);

    if (ctx->atlasTex) glDeleteTextures(1, &ctx->atlasTex);
    if (ctx->blitProgram) glDeleteProgram(ctx->blitProgram);
    if (ctx->atlasProgram) glDeleteProgram(ctx->atlasProgram);
    if (ctx->blitVbo) glDeleteBuffers(1, &ctx->blitVbo);
    if (ctx->atlasVbo) glDeleteBuffers(1, &ctx->atlasVbo);

    free(ctx->vertexBuf);
    free(ctx->bufA.atlasBuf);
    free(ctx->bufA.pieces);
    free(ctx->bufB.atlasBuf);
    free(ctx->bufB.pieces);
    free(ctx->scratchImages);
    free(ctx->scratchItems);
    free(ctx->scratchPlaceX);
    free(ctx->scratchPlaceY);
    free(ctx);
}
