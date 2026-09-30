#include "AssOverlay.h"

#include <android/log.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ass/ass.h"

// ============================================================================
// EFFECTS_ATLAS: native-side subtitle-onto-video-frame overlay for the GlEffect path.
//
// Draws subtitles directly into the caller-provided FBO, which wraps the decoded video frame's
// OWN texture (drawn in place) - there is no separate output texture and no full-frame video
// blit/copy pass: the video frame arrives already correct, and frames with no visible subtitle
// content do zero GL work at all (see the early return in nativeAssOverlayDraw once pieceCount is
// 0). This replaced an earlier design that always redrew the whole frame via a video-blit pass
// into a fresh output texture, because BaseGlShaderProgram (the higher-level media3 API that
// design used) always hands you a new, empty output framebuffer you must fill completely -
// AssGlShaderProgram now implements the raw GlShaderProgram interface directly instead, precisely
// to get this in-place behavior.
//
// Glyph pieces are shelf-packed into persistent, reused GPU texture atlas pages - normally just
// one, more only when a heavy typesetting frame doesn't fit in a single max-size texture - so
// subtitle rendering is one batched draw call per page per frame.
//
// ass_render_frame runs synchronously, inline, on this call (the GL thread) - there is no worker
// thread. This replaced an earlier design that ran it on a dedicated worker thread with a bounded
// wait, specifically to guarantee video delivery could never stall on a slow subtitle frame; that
// design added real complexity (cross-thread buffer handoff, a mutex held across GPU calls that
// caused measurable contention, a wait budget that was awkward to tune) for a guarantee this
// simpler synchronous design does not have. Trade-off, not a free win: a pathologically
// slow/complex subtitle frame (heavy \move/\t/karaoke) CAN stall video frame delivery here, for as
// long as ass_render_frame + the atlas pack/upload take. In exchange, every frame is trivially,
// always frame-accurate - there is no way for a "stale atlas" state to exist at all.
//
// No cross-thread synchronization at all in this file: the only concurrency concern is the JVM
// side's `Ass.lock` (see AssRender.kt's drawOverlayFrame), which this is always called from within,
// exactly like the existing synchronous renderFrame() path - it protects against concurrent track
// mutation (AssTrack.readChunk) from another thread, not against anything in this file.
// ============================================================================

#define ASS_OVERLAY_ATLAS_PADDING 2 // one zeroed texel on each side of a piece
#define ASS_OVERLAY_ATLAS_GROW_STEP 256
#define ASS_OVERLAY_VERTEX_FLOATS 8 // x,y,u,v,r,g,b,a
#define ASS_OVERLAY_LOG_TAG "AssOverlay"
// Anything at or above this is logged as a candidate culprit for a slow frame; below it is normal
// jitter not worth the logcat noise.
#define ASS_OVERLAY_SLOW_LOG_MS 2

static inline long long assOverlayNowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

// Draws every currently-visible subtitle piece of one atlas page in one batched call. Colors vary
// per piece so they travel as a per-vertex attribute (not a uniform, since one draw call spans many
// differently-colored pieces). Output is fully premultiplied (by both opacity and glyph coverage)
// so it can be composited with a standard premultiplied "over" blend function directly onto the
// video texture.
static const char* kAssOverlayVertexShader =
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

static const char* kAssOverlayFragmentShader =
    "precision mediump float;\n"
    // highp: mediump is fp16 on most mobile GPUs, which can't address individual texels of a
    // multi-thousand-texel atlas (UVs in [0.5, 1) of a 4096 atlas already step by 2 texels), making pieces
    // sample their neighbors/padding instead of their own bitmap.
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "varying highp vec2 v_TexCoord;\n"
    "#else\n"
    "varying vec2 v_TexCoord;\n"
    "#endif\n"
    "varying vec4 v_Color;\n"
    "uniform sampler2D u_Texture;\n"
    "void main() {\n"
    "    float coverage = texture2D(u_Texture, v_TexCoord).a;\n"
    "    float a = v_Color.a * coverage;\n"
    "    gl_FragColor = vec4(v_Color.rgb * a, a);\n"
    "}\n";


// Allocates the context and its GL objects. Must be called on the GL thread with the context current.
static AssOverlayContext* assOverlayCreate(void) {
    AssOverlayContext* ctx = (AssOverlayContext*) calloc(1, sizeof(AssOverlayContext));
    if (ctx == NULL)
    {
        __android_log_print(ANDROID_LOG_INFO, ASS_OVERLAY_LOG_TAG, "Failed to allocate AssOverlayContext");
        return NULL;
    }

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &ctx->maxAtlasSize);

    const char* version = (const char*) glGetString(GL_VERSION);
    int major = 0, minor = 0;
    if (version) {
        const char* ver = version;
        while (!isdigit((unsigned char) *ver) && *ver != '\0')
            ver++;
        if (sscanf(ver, "%d.%d", &major, &minor) != 2) {
            major = 0;
            __android_log_print(ANDROID_LOG_INFO, ASS_OVERLAY_LOG_TAG, "Invalid GL_VERSION string %s", version);
        }
    }
    ctx->bIsGles3 = major >= 3;
    __android_log_print(ANDROID_LOG_INFO, ASS_OVERLAY_LOG_TAG, "GL_VERSION=%s, max texture size %d, %s path",
        version ? version : "?", ctx->maxAtlasSize, ctx->bIsGles3 ? "GLES3 PBO/R8" : "GLES2");

    ctx->atlasProgram = assOverlayLinkProgram(kAssOverlayVertexShader, kAssOverlayFragmentShader);
    ctx->atlasAPosition = glGetAttribLocation(ctx->atlasProgram, "a_Position");
    ctx->atlasATexCoord = glGetAttribLocation(ctx->atlasProgram, "a_TexCoord");
    ctx->atlasAColor = glGetAttribLocation(ctx->atlasProgram, "a_Color");
    ctx->atlasUTexture = glGetUniformLocation(ctx->atlasProgram, "u_Texture");
    glGenBuffers(1, &ctx->atlasVbo);
    return ctx;
}

// Frees the context and deletes its GL objects. Must be called on the GL thread with the context current.
static void assOverlayDestroy(AssOverlayContext* ctx) {
    if (ctx == NULL) return;

    for (int i = 0; i < ctx->pageCap; i++) {
        if (ctx->pages[i].tex) glDeleteTextures(1, &ctx->pages[i].tex);
    }
    if (ctx->atlasProgram) glDeleteProgram(ctx->atlasProgram);
    if (ctx->atlasVbo) glDeleteBuffers(1, &ctx->atlasVbo);
    if (ctx->pbo) glDeleteBuffers(1, &ctx->pbo);

    free(ctx->pages);
    free(ctx->vertexBuf);
    free(ctx->atlasBuf);
    free(ctx->pieces);
    free(ctx);
}


static GLuint assOverlayCompileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        __android_log_print(ANDROID_LOG_ERROR, ASS_OVERLAY_LOG_TAG, "shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint assOverlayLinkProgram(const char* vsSrc, const char* fsSrc) {
    GLuint vs = assOverlayCompileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = assOverlayCompileShader(GL_FRAGMENT_SHADER, fsSrc);
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
        __android_log_print(ANDROID_LOG_ERROR, ASS_OVERLAY_LOG_TAG, "program link error: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}


static void assOverlayEnsureBuffer(AssOverlayContext* ctx, int atlasBytesNeeded, int pieceCountNeeded) {
    if (atlasBytesNeeded > ctx->atlasBufCap) {
        unsigned char* grown = (unsigned char*) realloc(ctx->atlasBuf, (size_t) atlasBytesNeeded);
        if (grown) {
            ctx->atlasBuf = grown;
            ctx->atlasBufCap = atlasBytesNeeded;
        }
    }
    if (pieceCountNeeded > ctx->pieceCap) {
        AssOverlayPiece* grown = (AssOverlayPiece*) realloc(ctx->pieces, (size_t) pieceCountNeeded * sizeof(AssOverlayPiece));
        if (grown) {
            ctx->pieces = grown;
            ctx->pieceCap = pieceCountNeeded;
        }
    }
}

// Makes page index `page` usable, zero-initializing any newly allocated entries (tex == 0 means
// "no GPU texture yet"). Returns 0 on OOM.
static int assOverlayEnsurePage(AssOverlayContext* ctx, int page) {
    if (page < ctx->pageCap) return 1;
    int newCap = ctx->pageCap ? ctx->pageCap * 2 : 2;
    if (newCap <= page) newCap = page + 1;
    AssOverlayPage* grown = (AssOverlayPage*) realloc(ctx->pages, (size_t) newCap * sizeof(AssOverlayPage));
    if (!grown) return 0;
    memset(grown + ctx->pageCap, 0, (size_t) (newCap - ctx->pageCap) * sizeof(AssOverlayPage));
    ctx->pages = grown;
    ctx->pageCap = newCap;
    return 1;
}

static int assOverlayCountImages(ASS_Image* images) {
    int count = 0;
    for (ASS_Image* img = images; img != NULL; img = img->next) {
        count++;
    }
    return count;
}

// Shelf-packs `image`'s pieces, in their ORIGINAL libass list order, into as many atlas pages as
// needed: overlapping same-position shadow/border/fill layers must be painted in that order for
// the "over" blending below to composite them correctly, which is only guaranteed if every page
// holds a contiguous run of the list. Only computes placement; pixels are copied per page in
// assOverlayUpload.
static void assOverlayPack(AssOverlayContext* ctx, ASS_Image* image) {
    ctx->pieceCount = 0;
    ctx->pageCount = 0;

    int count = assOverlayCountImages(image);
    if (count <= 0) return;

    assOverlayEnsureBuffer(ctx, 0, count);
    if (ctx->pieceCap < count) return; // OOM: nothing drawn this frame

    int maxSize = ctx->maxAtlasSize;

    // Shelf width: aim for a roughly square atlas (like mpv's packer) instead of filling shelves
    // out to maxSize. maxSize is GL_MAX_TEXTURE_SIZE (often 16384), so shelves that wide turn a row
    // of large signs into a ~16384 x 1080 staging buffer + upload per changed frame. Rounded to a
    // power of two so the page texture's size stays stable across frames instead of reallocating.
    long long area = 0;
    int widestPw = 0;
    for (ASS_Image* img = image; img != NULL; img = img->next) {
        if (img->w <= 0 || img->h <= 0) continue;
        int pw = img->w + ASS_OVERLAY_ATLAS_PADDING;
        area += (long long) pw * (img->h + ASS_OVERLAY_ATLAS_PADDING);
        if (pw > widestPw) widestPw = pw;
    }
    int shelfW = 256;
    while ((long long) shelfW * shelfW < area && shelfW < maxSize) shelfW *= 2;
    while (shelfW < widestPw && shelfW < maxSize) shelfW *= 2;
    if (shelfW > maxSize) shelfW = maxSize;

    int page = -1;
    int shelfX = 0, shelfY = 0, shelfH = 0;
    int placedCount = 0;
    for (ASS_Image* img = image; img != NULL; img = img->next) {
        if (img->w <= 0 || img->h <= 0) continue;
        int pw = img->w + ASS_OVERLAY_ATLAS_PADDING;
        int ph = img->h + ASS_OVERLAY_ATLAS_PADDING;
        if (pw > shelfW || ph > maxSize) {
            // Single piece larger than a whole page: cannot place (needs a frame bigger than the
            // GPU's max texture size, so not reachable in practice).
            __android_log_print(ANDROID_LOG_WARN, ASS_OVERLAY_LOG_TAG,
                "dropping %dx%d image: larger than max atlas size %d", img->w, img->h, maxSize);
            continue;
        }
        if (page >= 0 && shelfX + pw > shelfW) {
            shelfY += shelfH;
            shelfX = 0;
            shelfH = 0;
        }
        if (page < 0 || shelfY + ph > maxSize) {
            // Current page full (or none yet): start a new one at the current list position.
            if (!assOverlayEnsurePage(ctx, page + 1)) break; // OOM: draw what was placed so far
            page++;
            ctx->pages[page].firstPiece = placedCount;
            ctx->pages[page].pieceCount = 0;
            ctx->pages[page].usedW = 0;
            ctx->pages[page].usedH = 0;
            shelfX = shelfY = shelfH = 0;
        }

        AssOverlayPage* pg = &ctx->pages[page];
        AssOverlayPiece* p = &ctx->pieces[placedCount];
        // Inset by one texel inside its padded slot: the slot's outer ring is zeroed at upload time.
        p->atlasX = shelfX + 1;
        p->atlasY = shelfY + 1;
        p->dstX = img->dst_x;
        p->dstY = img->dst_y;
        p->w = img->w;
        p->h = img->h;
        p->r = ((img->color >> 24) & 0xFF) / 255.0f;
        p->g = ((img->color >> 16) & 0xFF) / 255.0f;
        p->b = ((img->color >> 8) & 0xFF) / 255.0f;
        p->a = (0xFF - (img->color & 0xFF)) / 255.0f;
        p->bitmap = img->bitmap;
        p->stride = img->stride;
        placedCount++;
        pg->pieceCount++;

        shelfX += pw;
        if (ph > shelfH) shelfH = ph;
        if (shelfX > pg->usedW) pg->usedW = shelfX;
        if (shelfY + shelfH > pg->usedH) pg->usedH = shelfY + shelfH;
    }

    ctx->pageCount = page + 1;
    ctx->pieceCount = placedCount;
}

// Writes one page's pieces into `dst` (a usedW x usedH, stride == usedW buffer): each piece's
// (stride-corrected) pixels plus a zeroed 1-texel ring around it, inside its padded slot. With
// GL_LINEAR that ring is the only thing outside the piece a sample can ever reach, and it must read
// as "no coverage" - so zeroing just the ring replaces memset'ing the whole buffer. The rest (shelf
// gaps) may hold stale bytes, but is never sampled. Writes are strictly sequential, row by row, so
// this is also fine for write-combined (mapped PBO) memory.
static void assOverlayFillPage(const AssOverlayContext* ctx, const AssOverlayPage* pg, unsigned char* buf) {
    const size_t stride = (size_t) pg->usedW;
    for (int i = pg->firstPiece; i < pg->firstPiece + pg->pieceCount; i++) {
        const AssOverlayPiece* p = &ctx->pieces[i];
        unsigned char* dst = buf + (size_t) p->atlasY * stride + p->atlasX;
        memset(dst - stride - 1, 0, (size_t) p->w + 2);
        for (int row = 0; row < p->h; row++) {
            unsigned char* line = dst + (size_t) row * stride;
            line[-1] = 0;
            memcpy(line, p->bitmap + (size_t) row * p->stride, (size_t) p->w);
            line[p->w] = 0;
        }
        memset(dst + (size_t) p->h * stride - 1, 0, (size_t) p->w + 2);
    }
}

// Binds pg's texture, (re)allocating it first if it's missing or too small for the last pack.
static void assOverlayBindPageTexture(AssOverlayContext* ctx, AssOverlayPage* pg) {
    if (pg->tex != 0 && pg->usedW <= pg->capW && pg->usedH <= pg->capH) {
        glBindTexture(GL_TEXTURE_2D, pg->tex);
        return;
    }
    // An overflowing dimension gets 1/8 headroom on top of what's needed, so a page that keeps
    // creeping up by a few rows doesn't reallocate (and re-page-in) a multi-MB texture each time.
    int newW = pg->capW >= pg->usedW ? pg->capW : pg->usedW + pg->usedW / 8;
    int newH = pg->capH >= pg->usedH ? pg->capH : pg->usedH + pg->usedH / 8;
    newW = ((newW + ASS_OVERLAY_ATLAS_GROW_STEP - 1) / ASS_OVERLAY_ATLAS_GROW_STEP) * ASS_OVERLAY_ATLAS_GROW_STEP;
    newH = ((newH + ASS_OVERLAY_ATLAS_GROW_STEP - 1) / ASS_OVERLAY_ATLAS_GROW_STEP) * ASS_OVERLAY_ATLAS_GROW_STEP;
    if (newW > ctx->maxAtlasSize) newW = ctx->maxAtlasSize;
    if (newH > ctx->maxAtlasSize) newH = ctx->maxAtlasSize;
    if (pg->tex) glDeleteTextures(1, &pg->tex);
    glGenTextures(1, &pg->tex);
    glBindTexture(GL_TEXTURE_2D, pg->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // GL_LINEAR, not GL_NEAREST: whenever AssHandlerConfig.maxRenderPixels downscales rendering,
    // this atlas is drawn magnified back up to the actual frame size, and GL_NEAREST under
    // magnification looks blocky. At 1:1 every fragment samples exactly a texel center, so
    // GL_LINEAR is exact there too. The zeroed ring around each piece keeps it from bleeding into a
    // neighboring packed piece.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (ctx->bIsGles3) {
        // GL_R8 is a native format everywhere; legacy GL_ALPHA is emulated by several mobile
        // drivers (converted on the CPU to a wider format during every upload). The swizzle keeps
        // the shader's `.a` read unchanged.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, newW, newH);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, newW, newH, 0, GL_ALPHA, GL_UNSIGNED_BYTE, NULL);
    }
    pg->capW = newW;
    pg->capH = newH;
}

// Uploads every page of the last pack. GLES3: pieces are written straight into a mapped pixel
// unpack buffer (the one unavoidable CPU copy), and glTexSubImage2D then sources from that buffer,
// so the driver transfers it GPU-side asynchronously instead of synchronously copying client
// memory again. GLES2 (or if mapping fails): the same via a CPU staging buffer. Only called when
// assOverlayPack just ran and produced at least one piece.
static void assOverlayUpload(AssOverlayContext* ctx) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int pi = 0; pi < ctx->pageCount; pi++) {
        AssOverlayPage* pg = &ctx->pages[pi];
        size_t bytes = (size_t) pg->usedW * (size_t) pg->usedH;
        assOverlayBindPageTexture(ctx, pg);
        GLenum format = ctx->bIsGles3 ? GL_RED : GL_ALPHA;

        int uploaded = 0;
        if (ctx->bIsGles3) {
            if (!ctx->pbo) glGenBuffers(1, &ctx->pbo);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, ctx->pbo);
            // Sized to the largest page capacity seen, not to this page's exact used area: drivers
            // only recycle orphaned storage of the SAME size, so a size that varied with every
            // pack meant a fresh multi-MB allocation (and its page faults) on each change.
            size_t capBytes = (size_t) pg->capW * (size_t) pg->capH;
            if (ctx->pboSize < capBytes) ctx->pboSize = capBytes;
            // Orphan: the driver hands out fresh storage if the GPU is still reading the previous
            // frame's upload from this buffer, so mapping never stalls on it.
            glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr) ctx->pboSize, NULL, GL_STREAM_DRAW);
            unsigned char* mapped = (unsigned char*) glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr) bytes,
                GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
            if (mapped) {
                assOverlayFillPage(ctx, pg, mapped);
                if (glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER)) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pg->usedW, pg->usedH, format, GL_UNSIGNED_BYTE, (const void*) 0);
                    uploaded = 1;
                }
            }
            // Must be unbound before any client-memory upload (ours below, or media3's own).
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        }
        if (!uploaded) {
            assOverlayEnsureBuffer(ctx, (int) bytes, 0);
            if ((size_t) ctx->atlasBufCap < bytes) { // OOM: draw only the pages uploaded so far
                ctx->pageCount = pi;
                ctx->pieceCount = pg->firstPiece;
                break;
            }
            assOverlayFillPage(ctx, pg, ctx->atlasBuf);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pg->usedW, pg->usedH, format, GL_UNSIGNED_BYTE, ctx->atlasBuf);
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

// changed == 1 from libass means only positions moved: the exact same bitmaps (same pointer, size,
// stride and color) in the same order. The uploaded atlas is then still valid as-is, so only the
// pieces' destinations need refreshing - no pack, no upload. Returns 0 if the list doesn't line up
// with the current pieces (e.g. a piece was dropped or truncated by OOM last time), in which case
// the caller does a full repack instead.
static int assOverlayUpdatePositions(AssOverlayContext* ctx, ASS_Image* image) {
    int i = 0;
    for (ASS_Image* img = image; img != NULL; img = img->next) {
        if (img->w <= 0 || img->h <= 0) continue;
        if (i >= ctx->pieceCount) return 0;
        AssOverlayPiece* p = &ctx->pieces[i];
        if (p->bitmap != img->bitmap || p->w != img->w || p->h != img->h) return 0;
        p->dstX = img->dst_x;
        p->dstY = img->dst_y;
        i++;
    }
    return i == ctx->pieceCount;
}

// Rebuilds the vertex buffer from ctx->pieces[] (positions + current page UV normalization).
static void assOverlayBuildVertices(AssOverlayContext* ctx) {
    if (ctx->pieceCount <= 0) return;
    int neededFloats = ctx->pieceCount * 6 * ASS_OVERLAY_VERTEX_FLOATS;
    if (neededFloats > ctx->vertexCapFloats) {
        float* grown = (float*) realloc(ctx->vertexBuf, (size_t) neededFloats * sizeof(float));
        if (grown) {
            ctx->vertexBuf = grown;
            ctx->vertexCapFloats = neededFloats;
        }
    }
    if (ctx->vertexBuf == NULL || ctx->vertexCapFloats < neededFloats || ctx->renderWidth <= 0 || ctx->renderHeight <= 0) {
        ctx->pieceCount = 0; // can't build vertices for what we just packed: nothing to draw this frame
        ctx->pageCount = 0;
        return;
    }

    int vi = 0;
    for (int pi = 0; pi < ctx->pageCount; pi++) {
        AssOverlayPage* pg = &ctx->pages[pi];
        for (int i = pg->firstPiece; i < pg->firstPiece + pg->pieceCount; i++) {
            AssOverlayPiece* p = &ctx->pieces[i];
            float x0 = (float) p->dstX;
            float y0 = (float) p->dstY;
            float x1 = x0 + (float) p->w;
            float y1 = y0 + (float) p->h;
            // Clip-space Y-flip: libass y is top-down, GL clip y is bottom-up. Dividing by
            // renderWidth/renderHeight (libass's own render target, which maxRenderPixels may have
            // made smaller than the actual frame) rather than the frame's own pixel size is what
            // makes the downscale-then-upscale-for-free technique work: clip space is
            // resolution-independent, so this maps correctly onto the actual (possibly larger)
            // frame this gets drawn into regardless.
            float cx0 = (x0 / (float) ctx->renderWidth) * 2.0f - 1.0f;
            float cx1 = (x1 / (float) ctx->renderWidth) * 2.0f - 1.0f;
            float cy0 = 1.0f - (y0 / (float) ctx->renderHeight) * 2.0f;
            float cy1 = 1.0f - (y1 / (float) ctx->renderHeight) * 2.0f;

            // Exact piece rect: the quad covers w x h pixels and samples exactly w x h texels, so at
            // 1:1 each fragment hits a texel center. (No inset: shrinking the UV rect would stretch
            // w-1 texels over w pixels and blur every glyph.)
            float u0 = (float) p->atlasX / (float) pg->capW;
            float u1 = (float) (p->atlasX + p->w) / (float) pg->capW;
            float v0 = (float) p->atlasY / (float) pg->capH;
            float v1 = (float) (p->atlasY + p->h) / (float) pg->capH;

            // Two triangles, matching the same paint (list) order pieces were packed in.
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
            vi += 6 * ASS_OVERLAY_VERTEX_FLOATS;
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, ctx->atlasVbo);
    // Buffer orphaning (full glBufferData respecification) is the standard pattern for per-frame-
    // changing vertex streams; avoids read-after-write hazards vs. glBufferSubData.
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) vi * sizeof(float), ctx->vertexBuf, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}


// Called every video frame, on the GL thread, from AssRender.kt's drawOverlayFrame (itself called
// from AssGlShaderProgram.queueInputFrame). `fbo` wraps the frame's own decoded-video texture -
// this draws directly into it, in place; the caller forwards that same texture downstream
// unmodified, so anything this function doesn't touch (i.e. every frame with no visible subtitle
// content) costs nothing beyond the ass_render_frame call itself.
jlong nativeAssOverlayDraw(JNIEnv* env, jclass clazz, jlong overlay, jlong render, jlong track,
    jint fbo, jint frameWidth, jint frameHeight, jint renderWidth, jint renderHeight, jlong timeMs) {
    if (!render || !track) return overlay;
    AssOverlayContext* ctx = (AssOverlayContext*) overlay;
    if (ctx == NULL) {
        ctx = assOverlayCreate();
        if (ctx == NULL) return 0;
    }

    if (frameWidth != ctx->frameW || frameHeight != ctx->frameH) {
        ctx->frameW = frameWidth;
        ctx->frameH = frameHeight;
        ass_set_storage_size((ASS_Renderer*) render, frameWidth, frameHeight);
        __android_log_print(ANDROID_LOG_INFO, ASS_OVERLAY_LOG_TAG, "ass_set_storage_size(%d, %d)", frameWidth, frameHeight);
    }
    if (renderWidth != ctx->renderWidth || renderHeight != ctx->renderHeight) {
        ctx->renderWidth = renderWidth;
        ctx->renderHeight = renderHeight;
        ass_set_frame_size((ASS_Renderer*) render, renderWidth, renderHeight);
        __android_log_print(ANDROID_LOG_INFO, ASS_OVERLAY_LOG_TAG, "ass_set_frame_size(%d, %d)", renderWidth, renderHeight);
    }

    long long t0 = assOverlayNowMs();
    int changed = 0;
    ASS_Image* image = ass_render_frame((ASS_Renderer*) render, (ASS_Track*) track, timeMs, &changed);
    long long renderMs = assOverlayNowMs() - t0;
    if (renderMs >= ASS_OVERLAY_SLOW_LOG_MS) {
        __android_log_print(ANDROID_LOG_WARN, ASS_OVERLAY_LOG_TAG,
            "ass_render_frame(timeMs=%lld) took %lldms (changed=%d)", (long long) timeMs, renderMs, changed);
    }

    if (changed == 1 && ctx->pieceCount > 0 && assOverlayUpdatePositions(ctx, image)) {
        // Positions only: atlas pages are still valid, just move the quads.
        long long t1 = assOverlayNowMs();
        assOverlayBuildVertices(ctx);
        long long buildMs = assOverlayNowMs() - t1;
        if (buildMs >= ASS_OVERLAY_SLOW_LOG_MS) {
            __android_log_print(ANDROID_LOG_WARN, ASS_OVERLAY_LOG_TAG,
                "assOverlayBuildVertices(timeMs=%lld, positions only) took %lldms", (long long) timeMs, buildMs);
        }
    } else if (changed) {
        long long t1 = assOverlayNowMs();
        assOverlayPack(ctx, image);
        long long packMs = assOverlayNowMs() - t1;
        if (packMs >= ASS_OVERLAY_SLOW_LOG_MS) {
            __android_log_print(ANDROID_LOG_WARN, ASS_OVERLAY_LOG_TAG,
                "assOverlayPack(timeMs=%lld) took %lldms", (long long) timeMs, packMs);
        }

        if (ctx->pieceCount > 0) {
            long long t2 = assOverlayNowMs();
            assOverlayUpload(ctx);
            long long t3 = assOverlayNowMs();
            assOverlayBuildVertices(ctx);
            long long t4 = assOverlayNowMs();
            if (t4 - t2 >= ASS_OVERLAY_SLOW_LOG_MS) {
                long long bytes = 0;
                for (int pi = 0; pi < ctx->pageCount; pi++) {
                    bytes += (long long) ctx->pages[pi].usedW * ctx->pages[pi].usedH;
                }
                __android_log_print(ANDROID_LOG_WARN, ASS_OVERLAY_LOG_TAG,
                    "upload(timeMs=%lld, pieces=%d, pages=%d, %lldKB, %s) took %lldms, vertices %lldms",
                    (long long) timeMs, ctx->pieceCount, ctx->pageCount, bytes / 1024,
                    ctx->bIsGles3 ? "PBO/R8" : "GLES2/ALPHA", t3 - t2, t4 - t3);
            }
        }
    }

    // pieceCount is sticky across changed==0 frames (same visible content as last time): still
    // needs to be drawn every single call, since each call's `fbo` wraps a DIFFERENT decoded video
    // frame that doesn't have the subtitle on it yet - only skip when there is truly nothing to
    // draw, which does cost nothing beyond the ass_render_frame call above.
    if (ctx->pieceCount <= 0 || ctx->pageCount <= 0) return (jlong) ctx;

    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) fbo);
    glViewport(0, 0, frameWidth, frameHeight);

    glEnable(GL_BLEND);
    // Fragment shader outputs fully premultiplied color (opacity * coverage), so blend with a
    // standard premultiplied "over": GL_ONE (not GL_SRC_ALPHA) for the source factor. The video's
    // own alpha channel (if any) is left alone: GL_ONE_MINUS_SRC_ALPHA on the destination factor
    // only affects color, not alpha, for this blend equation's default (GL_FUNC_ADD) on RGB/A alike
    // - acceptable here since the video frame's output alpha isn't read downstream regardless.
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(ctx->atlasProgram);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->atlasVbo);
    const GLsizei stride = ASS_OVERLAY_VERTEX_FLOATS * sizeof(GLfloat);
    glEnableVertexAttribArray((GLuint) ctx->atlasAPosition);
    glVertexAttribPointer((GLuint) ctx->atlasAPosition, 2, GL_FLOAT, GL_FALSE, stride, (void*) 0);
    glEnableVertexAttribArray((GLuint) ctx->atlasATexCoord);
    glVertexAttribPointer((GLuint) ctx->atlasATexCoord, 2, GL_FLOAT, GL_FALSE, stride, (void*) (2 * sizeof(GLfloat)));
    glEnableVertexAttribArray((GLuint) ctx->atlasAColor);
    glVertexAttribPointer((GLuint) ctx->atlasAColor, 4, GL_FLOAT, GL_FALSE, stride, (void*) (4 * sizeof(GLfloat)));
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(ctx->atlasUTexture, 0);
    // One draw per page, in page order: each page is a contiguous run of the libass list, so this
    // paints every piece in exactly libass's order.
    for (int pi = 0; pi < ctx->pageCount; pi++) {
        AssOverlayPage* pg = &ctx->pages[pi];
        glBindTexture(GL_TEXTURE_2D, pg->tex);
        glDrawArrays(GL_TRIANGLES, pg->firstPiece * 6, pg->pieceCount * 6);
    }

    // Leave GL state the way media3's own shader programs expect it, rather than trying to save and
    // precisely restore whatever was bound before (this IS the whole effect for this frame, same as
    // every other GlShaderProgram implementation - there is nothing "underneath" to preserve).
    glDisable(GL_BLEND);
    glDisableVertexAttribArray((GLuint) ctx->atlasAPosition);
    glDisableVertexAttribArray((GLuint) ctx->atlasATexCoord);
    glDisableVertexAttribArray((GLuint) ctx->atlasAColor);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prevFbo);

    return (jlong) ctx;
}

void nativeAssOverlayRelease(JNIEnv* env, jclass clazz, jlong overlay) {
    assOverlayDestroy((AssOverlayContext*) overlay);
}
