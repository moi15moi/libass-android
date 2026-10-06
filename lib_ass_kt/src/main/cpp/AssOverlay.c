#include "AssOverlay.h"

#include <android/log.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "GLES3/gl3.h"
#include "ass/ass.h"

// ============================================================================
// EFFECTS_ATLAS: native-side subtitle-onto-video-frame overlay for the GlEffect path.
//
// Draws subtitles directly into the caller-provided FBO, which wraps the decoded video frame's
// OWN texture (drawn in place): there is no separate output texture and no full-frame copy, and
// frames with no visible subtitle content do no GL work at all.
//
// Glyph pieces are skyline-packed into persistent, reused GPU texture atlas pages - normally just
// one, more only when a heavy typesetting frame doesn't fit in a single max-size texture - so
// subtitle rendering is one batched draw call per page per frame. A bitmap libass hands out
// several times in one frame is stored once per page.
//
// ass_render_frame runs synchronously on this call (the GL thread), so every frame is
// frame-accurate, at the cost that a pathologically slow subtitle frame can stall video delivery
// for as long as ass_render_frame + the atlas pack/upload take.
//
// Always called from within the JVM side's `Ass.lock` (see AssRender.kt's drawOverlayFrame), which
// protects against concurrent track mutation; nothing in this file needs its own synchronization.
// ============================================================================

#define ASS_OVERLAY_LOG_TAG "AssOverlay"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, ASS_OVERLAY_LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, ASS_OVERLAY_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, ASS_OVERLAY_LOG_TAG, __VA_ARGS__)

// Page texture capacities are rounded up to a multiple of this.
#define ASS_OVERLAY_ATLAS_GROW_STEP 256
// Smallest atlas width the packer starts from before doubling to fit the frame's pieces.
#define ASS_OVERLAY_MIN_PACK_WIDTH 256
// Initial piece capacity; it then doubles whenever a frame has more pieces than fit.
#define ASS_OVERLAY_MIN_PIECE_CAP 64
// Each piece is drawn as a quad made of two triangles.
#define ASS_OVERLAY_VERTICES_PER_PIECE 6
// Upper bound on waiting for the GPU to release a PBO (only hit if it is badly behind).
#define ASS_OVERLAY_PBO_WAIT_NS 100000000ULL
// Anything at or above this is logged as a candidate culprit for a slow frame; below it is normal
// jitter not worth the logcat noise.
#define ASS_OVERLAY_SLOW_LOG_MS 2

// Define to also log, for every slow frame, a per-stage microsecond breakdown of the atlas update
// (pack, texture allocation, PBO map, fill, upload, vertices) and its area stats (piece texels vs
// packed vs allocated). Off by default: it adds clock reads and an extra pass over the pieces.
// #define ASS_OVERLAY_PROFILE
#ifdef ASS_OVERLAY_PROFILE
#define ASS_OVERLAY_PROF(...) __VA_ARGS__
#else
#define ASS_OVERLAY_PROF(...)
#endif

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
    // Closest earlier piece showing the very same bitmap, or -1. When that copy is on the same page,
    // this piece samples its texels instead of being stored (and uploaded) again.
    int sameAs;
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

// One step of a page's skyline: the packed height over the columns [x, next node's x).
typedef struct {
    int x, y;
} AssOverlaySkyNode;

// A piece in packing order (see assOverlayCompareTallestFirst).
typedef struct {
    int h, w, piece;
} AssOverlayPackEntry;

typedef struct {
    float x, y; // clip space
    float u, v; // atlas texture coordinates
    uint8_t r, g, b, a; // normalized to [0, 1] by the attribute pointer
} AssOverlayVertex;

typedef struct {
    // GL objects, created on the first draw call.
    GLuint atlasProgram;
    GLint atlasAPosition;
    GLint atlasATexCoord;
    GLint atlasAColor;
    GLuint atlasVbo;
    int maxAtlasSize;
    bool isGles3; // context is GLES 3.0+: R8 textures + PBO uploads (see assOverlayUpload)
    // GL_PIXEL_UNPACK_BUFFER for page uploads (GLES3 only). Grow-only and never orphaned: on
    // ANGLE, orphaning a multi-MB buffer allocated and page-faulted brand-new storage on every
    // change, costing 10-18ms per upload. Instead it carries a fence for its last upload, and is
    // mapped unsynchronized once that fence has signaled. A single buffer is enough (that upload
    // is normally long done by the next changed frame), and every extra buffer would cost one more
    // expensive first-use allocation (~8-13ms for a heavy frame on ANGLE).
    GLuint pbo;
    size_t pboSize;
    GLsync pboFence;

    // Pages [0, pageCount) are in use; [pageCount, pageCap) keep their textures around for reuse.
    AssOverlayPage* pages;
    int pageCap;
    int pageCount;

    // The video frame's own pixel size == libass storage and frame size == GL viewport. Only
    // re-applied via ass_set_* when it actually changes.
    int frameW, frameH;

    // CPU-side staging for one page at a time (GLES2, or if mapping the PBO fails).
    unsigned char* atlasBuf;
    size_t atlasBufCap;

    AssOverlayPiece* pieces;
    int pieceCap;
    int pieceCount; // current visible piece count; sticky across changed==0 frames

    // Packer scratch, sized along with pieces[] (see assOverlayEnsurePieces).
    AssOverlayPackEntry* packOrder; // every piece, in packing order
    AssOverlaySkyNode* skyNodes; // skyline of the page being packed: each placed piece adds at most one node
    int* bitmapSlots; // hash table of piece indices (-1 = empty slot), see assOverlayLinkRepeats
    int bitmapSlotCap; // a power of two, at least twice pieceCap

    AssOverlayVertex* vertexBuf; // ASS_OVERLAY_VERTICES_PER_PIECE vertices per piece
    int vertexCap;
    int vboCap; // atlasVbo's allocated size, in vertices: grow-only, updated in place

#ifdef ASS_OVERLAY_PROFILE
    // Per-stage breakdown of the last assOverlayUpdate, in microseconds, and its texel counts.
    long long profPackUs, profTexAllocUs, profMapUs, profFillUs, profTexUs, profVertUs;
    long long profPieceArea, profUsedArea, profCapArea;
#endif
} AssOverlayContext;

static inline long long assOverlayNowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

#ifdef ASS_OVERLAY_PROFILE
static inline long long assOverlayNowUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}
#endif

static inline int assOverlayRoundUp(int value, int step) {
    return ((value + step - 1) / step) * step;
}

// libass emits zero-sized images; they have nothing to pack or draw.
static inline bool assOverlayIsEmpty(const ASS_Image* img) {
    return img->w <= 0 || img->h <= 0;
}

// libass packs a color as 0xRRGGBBTT, where TT is transparency (0 = opaque), not alpha.
static inline void assOverlayUnpackColor(uint32_t color, uint8_t* r, uint8_t* g, uint8_t* b, uint8_t* a) {
    *r = (uint8_t) (color >> 24);
    *g = (uint8_t) (color >> 16);
    *b = (uint8_t) (color >> 8);
    *a = (uint8_t) (0xFF - (color & 0xFF));
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
    // sample their neighbors instead of their own bitmap.
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

static GLuint assOverlayCompileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    if (shader == 0) return 0;
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        LOGE("shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint assOverlayLinkProgram(const char* vsSrc, const char* fsSrc) {
    GLuint vs = assOverlayCompileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = assOverlayCompileShader(GL_FRAGMENT_SHADER, fsSrc);
    if (vs == 0 || fs == 0) {
        if (vs != 0) glDeleteShader(vs);
        if (fs != 0) glDeleteShader(fs);
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
        LOGE("program link error: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

// Frees the context and deletes its GL objects. Must be called on the GL thread with the context current.
static void assOverlayDestroy(AssOverlayContext* ctx) {
    if (ctx == NULL) return;

    for (int pageIdx = 0; pageIdx < ctx->pageCap; pageIdx++) {
        if (ctx->pages[pageIdx].tex != 0) glDeleteTextures(1, &ctx->pages[pageIdx].tex);
    }
    if (ctx->atlasProgram != 0) glDeleteProgram(ctx->atlasProgram);
    if (ctx->atlasVbo != 0) glDeleteBuffers(1, &ctx->atlasVbo);
    if (ctx->pboFence != 0) glDeleteSync(ctx->pboFence);
    if (ctx->pbo != 0) glDeleteBuffers(1, &ctx->pbo);

    free(ctx->pages);
    free(ctx->vertexBuf);
    free(ctx->atlasBuf);
    free(ctx->pieces);
    free(ctx->packOrder);
    free(ctx->skyNodes);
    free(ctx->bitmapSlots);
    free(ctx);
}

// Allocates the context and its GL objects. Must be called on the GL thread with the context current.
// Returns NULL on failure.
static AssOverlayContext* assOverlayCreate(void) {
    AssOverlayContext* ctx = (AssOverlayContext*) calloc(1, sizeof(AssOverlayContext));
    if (ctx == NULL) {
        LOGE("Failed to allocate AssOverlayContext");
        return NULL;
    }

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &ctx->maxAtlasSize);

    // GLES guarantees GL_VERSION starts with "OpenGL ES <major>.<minor>".
    const char* version = (const char*) glGetString(GL_VERSION);
    int major = 0;
    if (version == NULL || sscanf(version, "OpenGL ES %d", &major) != 1) {
        major = 0;
    }
    ctx->isGles3 = major >= 3;
    LOGI("GL_VERSION=%s, max texture size %d, %s path",
        version != NULL ? version : "?", ctx->maxAtlasSize, ctx->isGles3 ? "GLES3 PBO/R8" : "GLES2");

    ctx->atlasProgram = assOverlayLinkProgram(kAssOverlayVertexShader, kAssOverlayFragmentShader);
    if (ctx->atlasProgram == 0) {
        assOverlayDestroy(ctx);
        return NULL;
    }
    ctx->atlasAPosition = glGetAttribLocation(ctx->atlasProgram, "a_Position");
    ctx->atlasATexCoord = glGetAttribLocation(ctx->atlasProgram, "a_TexCoord");
    ctx->atlasAColor = glGetAttribLocation(ctx->atlasProgram, "a_Color");
    // The atlas is always bound to texture unit 0, so this never needs setting again.
    glUseProgram(ctx->atlasProgram);
    glUniform1i(glGetUniformLocation(ctx->atlasProgram, "u_Texture"), 0);
    glUseProgram(0);
    glGenBuffers(1, &ctx->atlasVbo);
    return ctx;
}

// Makes room for `count` pieces (and their packer scratch and vertices). Capacity at least doubles
// on each growth, so a piece count that creeps up frame by frame only rarely reallocates. Returns
// false on OOM (the existing buffers are kept).
static bool assOverlayEnsurePieces(AssOverlayContext* ctx, int count) {
    if (count <= ctx->pieceCap) return true;
    int newCap = ctx->pieceCap > 0 ? ctx->pieceCap * 2 : ASS_OVERLAY_MIN_PIECE_CAP;
    if (newCap < count) newCap = count;
    AssOverlayPiece* grown = (AssOverlayPiece*) realloc(ctx->pieces, (size_t) newCap * sizeof(AssOverlayPiece));
    if (grown == NULL) return false;
    ctx->pieces = grown;
    AssOverlayPackEntry* grownOrder = (AssOverlayPackEntry*) realloc(ctx->packOrder, (size_t) newCap * sizeof(AssOverlayPackEntry));
    if (grownOrder == NULL) return false;
    ctx->packOrder = grownOrder;
    AssOverlaySkyNode* grownNodes = (AssOverlaySkyNode*) realloc(ctx->skyNodes, (size_t) (newCap + 2) * sizeof(AssOverlaySkyNode));
    if (grownNodes == NULL) return false;
    ctx->skyNodes = grownNodes;
    int newSlotCap = ctx->bitmapSlotCap > 0 ? ctx->bitmapSlotCap : 2 * ASS_OVERLAY_MIN_PIECE_CAP;
    while (newSlotCap < 2 * newCap) newSlotCap *= 2;
    int* grownSlots = (int*) realloc(ctx->bitmapSlots, (size_t) newSlotCap * sizeof(int));
    if (grownSlots == NULL) return false;
    ctx->bitmapSlots = grownSlots;
    ctx->bitmapSlotCap = newSlotCap;
    int newVertexCap = newCap * ASS_OVERLAY_VERTICES_PER_PIECE;
    AssOverlayVertex* grownVertices = (AssOverlayVertex*) realloc(ctx->vertexBuf, (size_t) newVertexCap * sizeof(AssOverlayVertex));
    if (grownVertices == NULL) return false;
    ctx->vertexBuf = grownVertices;
    ctx->vertexCap = newVertexCap;
    ctx->pieceCap = newCap;
    return true;
}

// Makes the CPU staging buffer at least `bytes` long, with 1/8 headroom (like the PBO) so a page
// that grows by a few rows doesn't reallocate it. Returns false on OOM (the existing buffer is kept).
static bool assOverlayEnsureStaging(AssOverlayContext* ctx, size_t bytes) {
    if (bytes <= ctx->atlasBufCap) return true;
    size_t newCap = bytes + bytes / 8;
    unsigned char* grown = (unsigned char*) realloc(ctx->atlasBuf, newCap);
    if (grown == NULL) return false;
    ctx->atlasBuf = grown;
    ctx->atlasBufCap = newCap;
    return true;
}

// Makes page index `page` usable, zero-initializing any newly allocated entries (tex == 0 means
// "no GPU texture yet"). Returns false on OOM.
static bool assOverlayEnsurePage(AssOverlayContext* ctx, int page) {
    if (page < ctx->pageCap) return true;
    int newCap = ctx->pageCap > 0 ? ctx->pageCap * 2 : 2;
    if (newCap <= page) newCap = page + 1;
    AssOverlayPage* grown = (AssOverlayPage*) realloc(ctx->pages, (size_t) newCap * sizeof(AssOverlayPage));
    if (grown == NULL) return false;
    memset(grown + ctx->pageCap, 0, (size_t) (newCap - ctx->pageCap) * sizeof(AssOverlayPage));
    ctx->pages = grown;
    ctx->pageCap = newCap;
    return true;
}

// Links every piece to the closest earlier piece showing the very same bitmap (same pixels, size
// and stride), or -1. libass hands out one cached bitmap several times per frame - layered copies
// of a sign, repeated effect strips - and a page stores such a bitmap only once.
static void assOverlayLinkRepeats(AssOverlayContext* ctx) {
    const uint32_t mask = (uint32_t) ctx->bitmapSlotCap - 1;
    memset(ctx->bitmapSlots, 0xFF, (size_t) ctx->bitmapSlotCap * sizeof(int)); // every slot -1
    for (int pieceIdx = 0; pieceIdx < ctx->pieceCount; pieceIdx++) {
        AssOverlayPiece* p = &ctx->pieces[pieceIdx];
        p->sameAs = -1;
        // Linear probing from a Fibonacci hash of the pointer. At most half the slots are ever
        // used, so the probe always ends on either a match or an empty slot.
        uint32_t slot = (uint32_t) (((uint64_t) (uintptr_t) p->bitmap * 0x9E3779B97F4A7C15ULL) >> 32) & mask;
        for (; ctx->bitmapSlots[slot] >= 0; slot = (slot + 1) & mask) {
            const AssOverlayPiece* q = &ctx->pieces[ctx->bitmapSlots[slot]];
            if (q->bitmap == p->bitmap && q->w == p->w && q->h == p->h && q->stride == p->stride) {
                p->sameAs = ctx->bitmapSlots[slot];
                break;
            }
        }
        // The slot keeps the latest copy, so the next repeat links to the closest one.
        ctx->bitmapSlots[slot] = pieceIdx;
    }
}

// Texels pieces[pieceIdx] adds to a page whose run starts at `first`: none if it repeats a bitmap
// already in the run.
static inline long long assOverlayRunTexels(const AssOverlayContext* ctx, int first, int pieceIdx) {
    const AssOverlayPiece* p = &ctx->pieces[pieceIdx];
    return p->sameAs >= first ? 0 : (long long) p->w * p->h;
}

// Tallest first, then widest: the skyline then fills the space beside tall pieces with shorter
// ones. Ties keep list order, so the layout is deterministic.
static int assOverlayCompareTallestFirst(const void* a, const void* b) {
    const AssOverlayPackEntry* p = (const AssOverlayPackEntry*) a;
    const AssOverlayPackEntry* q = (const AssOverlayPackEntry*) b;
    if (p->h != q->h) return q->h - p->h;
    if (p->w != q->w) return q->w - p->w;
    return p->piece - q->piece;
}

// Page width: aim for a roughly square page (like mpv's packer) instead of packing out to maxSize.
// maxSize is GL_MAX_TEXTURE_SIZE (often 16384), so a page that wide turns a row of large signs into
// a ~16384 x 1080 upload per changed frame. Rounded to a power of two so the page texture's size
// stays stable across frames instead of reallocating.
static int assOverlayPageWidth(long long area, int widest, int maxSize) {
    int width = ASS_OVERLAY_MIN_PACK_WIDTH;
    while ((long long) width * width < area && width < maxSize) width *= 2;
    while (width < widest && width < maxSize) width *= 2;
    return width < maxSize ? width : maxSize;
}

// A page's skyline is the top edge of everything packed into it so far: node i covers the columns
// [nodes[i].x, nodes[i + 1].x) up to height nodes[i].y, nodes[count] marks the page's right edge,
// and neighboring nodes always differ in height.
//
// Finds where a w x h piece rests lowest - then leftmost - with its left edge on a node and its top
// at most maxH. Returns that node (and the resting height), or -1 if the piece doesn't fit.
static int assOverlaySkyFind(const AssOverlaySkyNode* nodes, int count, int pageW, int maxH, int w, int h, int* restY) {
    int bestNode = -1;
    int bestY = maxH - h + 1; // resting this high or higher would overflow the page
    for (int nodeIdx = 0; nodeIdx < count && nodes[nodeIdx].x + w <= pageW; nodeIdx++) {
        // The piece rests on the highest node under it. Later candidates are further right, so only
        // a strictly lower one can win: stop scanning a candidate as soon as it can't.
        const int right = nodes[nodeIdx].x + w;
        int y = 0;
        for (int k = nodeIdx; nodes[k].x < right && y < bestY; k++) {
            if (nodes[k].y > y) y = nodes[k].y;
        }
        if (y < bestY) {
            bestY = y;
            bestNode = nodeIdx;
        }
    }
    *restY = bestY;
    return bestNode;
}

// Raises the skyline under a piece just placed on node `nodeIdx`: the columns it covers become one
// node at `top`, and the last node it only partly covers keeps its uncovered remainder.
static void assOverlaySkyPlace(AssOverlaySkyNode* nodes, int* count, int nodeIdx, int w, int top) {
    const int right = nodes[nodeIdx].x + w;
    int end = nodeIdx + 1; // first node starting at or past the piece's right edge
    while (nodes[end].x < right) end++;
    const bool partial = nodes[end].x > right;
    const int restY = nodes[end - 1].y;

    // Nodes [nodeIdx, end) collapse into nodeIdx, plus the remainder. The right edge marker moves too.
    const int kept = partial ? 2 : 1;
    memmove(&nodes[nodeIdx + kept], &nodes[end], (size_t) (*count + 1 - end) * sizeof(AssOverlaySkyNode));
    *count += kept - (end - nodeIdx);
    nodes[nodeIdx].y = top;
    if (partial) {
        nodes[nodeIdx + 1] = (AssOverlaySkyNode) {right, restY}; // below the piece's top: no merge
    } else if (nodeIdx + 1 < *count && nodes[nodeIdx + 1].y == top) {
        memmove(&nodes[nodeIdx + 1], &nodes[nodeIdx + 2], (size_t) (*count - nodeIdx - 1) * sizeof(AssOverlaySkyNode));
        (*count)--;
    }
    if (nodeIdx > 0 && nodes[nodeIdx - 1].y == top) {
        memmove(&nodes[nodeIdx], &nodes[nodeIdx + 1], (size_t) (*count - nodeIdx) * sizeof(AssOverlaySkyNode));
        (*count)--;
    }
}

// Packs the run pieces[first, end) into one page: tallest first, each at the lowest then leftmost
// spot the skyline offers, and each distinct bitmap only once - a repeat of a bitmap already in the
// run samples that copy's texels. Placement order is free because only pieces[] order (libass list
// order) decides the paint order. Returns false if the run doesn't fit in one max-size page.
static bool assOverlayPackPage(AssOverlayContext* ctx, int first, int end, int* usedW, int* usedH) {
    long long area = 0;
    int widest = 0;
    for (int pieceIdx = first; pieceIdx < end; pieceIdx++) {
        area += assOverlayRunTexels(ctx, first, pieceIdx);
        if (ctx->pieces[pieceIdx].w > widest) widest = ctx->pieces[pieceIdx].w;
    }
    const int pageW = assOverlayPageWidth(area, widest, ctx->maxAtlasSize);

    // Height is the max texture size: the skyline keeps the packing as low as it can, so the
    // actually used height is read back from the result instead of guessed up front.
    AssOverlaySkyNode* nodes = ctx->skyNodes;
    int nodeCount = 1;
    nodes[0] = (AssOverlaySkyNode) {0, 0};
    nodes[1] = (AssOverlaySkyNode) {pageW, 0};
    *usedW = 0;
    *usedH = 0;
    for (int orderIdx = 0; orderIdx < ctx->pieceCount; orderIdx++) {
        const int pieceIdx = ctx->packOrder[orderIdx].piece;
        AssOverlayPiece* p = &ctx->pieces[pieceIdx];
        if (pieceIdx < first || pieceIdx >= end || p->sameAs >= first) continue;
        int y;
        const int nodeIdx = assOverlaySkyFind(nodes, nodeCount, pageW, ctx->maxAtlasSize, p->w, p->h, &y);
        if (nodeIdx < 0) return false;
        p->atlasX = nodes[nodeIdx].x;
        p->atlasY = y;
        assOverlaySkyPlace(nodes, &nodeCount, nodeIdx, p->w, y + p->h);
        if (p->atlasX + p->w > *usedW) *usedW = p->atlasX + p->w;
        if (y + p->h > *usedH) *usedH = y + p->h;
    }
    // In list order, so a repeat's copy (itself possibly a repeat) is always placed already.
    for (int pieceIdx = first; pieceIdx < end; pieceIdx++) {
        AssOverlayPiece* p = &ctx->pieces[pieceIdx];
        if (p->sameAs < first) continue;
        p->atlasX = ctx->pieces[p->sameAs].atlasX;
        p->atlasY = ctx->pieces[p->sameAs].atlasY;
    }
    return true;
}

// Collects `image`'s pieces into ctx->pieces[] in libass list order (which is also the draw
// order), then assigns their atlas placement. Only computes placement; pixels are copied per page
// in assOverlayUpload.
//
// Every page holds the longest run of the remaining list that assOverlayPackPage fits into it; a
// frame that fits one page is just the case where the first run is the whole list. Each page is
// drawn in its own call, so overlapping shadow/border/fill layers are only painted in libass's order
// if every page holds a contiguous run of the list - which rules out spreading a run across pages.
//
// Pieces are packed edge to edge, with no padding: they are always drawn 1:1 with GL_NEAREST, so a
// fragment only ever samples a texel inside its own piece.
static void assOverlayPack(AssOverlayContext* ctx, ASS_Image* image) {
    ctx->pieceCount = 0;
    ctx->pageCount = 0;

    int count = 0;
    for (ASS_Image* img = image; img != NULL; img = img->next) {
        if (!assOverlayIsEmpty(img)) count++;
    }
    if (count == 0) return;
    if (!assOverlayEnsurePieces(ctx, count)) return; // OOM: nothing drawn this frame

    const int maxSize = ctx->maxAtlasSize;
    int pieceCount = 0;
    for (ASS_Image* img = image; img != NULL; img = img->next) {
        if (assOverlayIsEmpty(img)) continue;
        if (img->w > maxSize || img->h > maxSize) {
            // Single piece larger than a whole page: cannot place (needs a frame bigger than the
            // GPU's max texture size, so not reachable in practice).
            LOGW("dropping %dx%d image: larger than max atlas size %d", img->w, img->h, maxSize);
            continue;
        }
        AssOverlayPiece* p = &ctx->pieces[pieceCount];
        p->dstX = img->dst_x;
        p->dstY = img->dst_y;
        p->w = img->w;
        p->h = img->h;
        p->color = img->color;
        p->bitmap = img->bitmap;
        p->stride = img->stride;
        ctx->packOrder[pieceCount] = (AssOverlayPackEntry) {img->h, img->w, pieceCount};
        pieceCount++;
    }
    ctx->pieceCount = pieceCount;
    if (pieceCount == 0) return;
    assOverlayLinkRepeats(ctx);
    // Sorted once for all pages: each page packs its run's pieces in this order.
    qsort(ctx->packOrder, (size_t) pieceCount, sizeof(AssOverlayPackEntry), assOverlayCompareTallestFirst);

    const long long pageArea = (long long) maxSize * maxSize;
    int first = 0;
    while (first < pieceCount) {
        // Start from the longest run whose distinct bitmaps don't add up to more than a whole page.
        long long runArea = assOverlayRunTexels(ctx, first, first);
        int end = first + 1;
        while (end < pieceCount && runArea + assOverlayRunTexels(ctx, first, end) <= pageArea) {
            runArea += assOverlayRunTexels(ctx, first, end);
            end++;
        }
        int usedW, usedH;
        while (!assOverlayPackPage(ctx, first, end, &usedW, &usedH)) {
            // Packing never fills a page completely: retry without the run's last 1/8 of texels (and
            // at least its last piece). A lone piece always fits, as oversized ones were dropped above.
            const long long target = runArea - runArea / 8;
            runArea = assOverlayRunTexels(ctx, first, first);
            int shorter = first + 1;
            while (shorter < end - 1 && runArea + assOverlayRunTexels(ctx, first, shorter) <= target) {
                runArea += assOverlayRunTexels(ctx, first, shorter);
                shorter++;
            }
            end = shorter;
        }

        if (!assOverlayEnsurePage(ctx, ctx->pageCount)) { // OOM: draw the pages packed so far
            ctx->pieceCount = first;
            return;
        }
        AssOverlayPage* pg = &ctx->pages[ctx->pageCount++];
        pg->firstPiece = first;
        pg->pieceCount = end - first;
        pg->usedW = usedW;
        pg->usedH = usedH;
        first = end;
    }
}

// Writes one page's pieces into `buf` (a usedW x usedH, stride == usedW buffer), piece by piece, one
// memcpy per piece row. The rest (gaps between pieces) may hold stale bytes, but is never sampled.
static void assOverlayFillPage(const AssOverlayContext* ctx, const AssOverlayPage* pg, unsigned char* buf) {
    const size_t stride = (size_t) pg->usedW;
    for (int pieceIdx = pg->firstPiece; pieceIdx < pg->firstPiece + pg->pieceCount; pieceIdx++) {
        const AssOverlayPiece* p = &ctx->pieces[pieceIdx];
        if (p->sameAs >= pg->firstPiece) continue; // an earlier copy on this page already holds these texels
        unsigned char* dst = buf + (size_t) p->atlasY * stride + p->atlasX;
        const unsigned char* src = p->bitmap;
        for (int row = 0; row < p->h; row++) {
            memcpy(dst, src, (size_t) p->w);
            dst += stride;
            src += p->stride;
        }
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
    newW = assOverlayRoundUp(newW, ASS_OVERLAY_ATLAS_GROW_STEP);
    newH = assOverlayRoundUp(newH, ASS_OVERLAY_ATLAS_GROW_STEP);
    if (newW > ctx->maxAtlasSize) newW = ctx->maxAtlasSize;
    if (newH > ctx->maxAtlasSize) newH = ctx->maxAtlasSize;
    if (pg->tex != 0) glDeleteTextures(1, &pg->tex);
    glGenTextures(1, &pg->tex);
    glBindTexture(GL_TEXTURE_2D, pg->tex);
    // GLES2 requires CLAMP_TO_EDGE for non-power-of-two textures (the capacity is only rounded to a
    // multiple of ASS_OVERLAY_ATLAS_GROW_STEP), otherwise the texture is incomplete and samples as black.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Pieces are always drawn 1:1, so every fragment samples exactly a texel center: GL_NEAREST
    // returns that texel and never reaches a neighboring packed piece.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (ctx->isGles3) {
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
// unpack buffer (the one unavoidable CPU copy) - a persistent, fenced one, mapped unsynchronized
// rather than orphaned (see AssOverlayContext.pbo) - and glTexSubImage2D then sources from that
// buffer, so the driver transfers it GPU-side asynchronously instead of synchronously copying client
// memory again. GLES2 (or if mapping fails): the same via a CPU staging buffer. Only called when
// assOverlayPack just ran and produced at least one piece.
static void assOverlayUpload(AssOverlayContext* ctx) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    GLenum format = ctx->isGles3 ? GL_RED : GL_ALPHA;
    for (int pageIdx = 0; pageIdx < ctx->pageCount; pageIdx++) {
        AssOverlayPage* pg = &ctx->pages[pageIdx];
        size_t bytes = (size_t) pg->usedW * (size_t) pg->usedH;
        ASS_OVERLAY_PROF(long long tBind = assOverlayNowUs();)
        assOverlayBindPageTexture(ctx, pg);
        ASS_OVERLAY_PROF(
            long long tA = assOverlayNowUs();
            ctx->profTexAllocUs += tA - tBind;
            ctx->profUsedArea += (long long) bytes;
            ctx->profCapArea += (long long) pg->capW * pg->capH;
            for (int i = pg->firstPiece; i < pg->firstPiece + pg->pieceCount; i++)
                ctx->profPieceArea += assOverlayRunTexels(ctx, pg->firstPiece, i);
        )

        bool uploaded = false;
        if (ctx->isGles3) {
            if (ctx->pbo == 0) glGenBuffers(1, &ctx->pbo);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, ctx->pbo);
            // The GPU must be done with this buffer's previous upload before it is overwritten. That
            // upload was issued at least one changed frame ago, so its fence has normally signaled.
            if (ctx->pboFence != 0) {
                glClientWaitSync(ctx->pboFence, GL_SYNC_FLUSH_COMMANDS_BIT, ASS_OVERLAY_PBO_WAIT_NS);
                glDeleteSync(ctx->pboFence);
                ctx->pboFence = 0;
            }
            // Grow-only, sized to the used area plus 1/8 headroom rather than to the page's (larger,
            // rounded-up) capacity: first use of new storage is the expensive part, so allocate as
            // little as possible while still not reallocating as the area creeps up.
            if (ctx->pboSize < bytes) {
                size_t newSize = bytes + bytes / 8;
                glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr) newSize, NULL, GL_STREAM_DRAW);
                ctx->pboSize = newSize;
            }
            // Unsynchronized the fence above already guarantees the GPU is done with it.
            unsigned char* mapped = (unsigned char*) glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr) bytes,
                GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
            ASS_OVERLAY_PROF(
                long long tB = assOverlayNowUs();
                ctx->profMapUs += tB - tA;
            )
            if (mapped != NULL) {
                assOverlayFillPage(ctx, pg, mapped);
                ASS_OVERLAY_PROF(
                    long long tC = assOverlayNowUs();
                    ctx->profFillUs += tC - tB;
                )
                if (glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER)) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pg->usedW, pg->usedH, format, GL_UNSIGNED_BYTE, (const void*) 0);
                    uploaded = true;
                    ctx->pboFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                }
                ASS_OVERLAY_PROF(ctx->profTexUs += assOverlayNowUs() - tC;)
            }
            // Must be unbound before any client-memory upload (ours below, or media3's own).
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        }
        if (!uploaded) {
            if (!assOverlayEnsureStaging(ctx, bytes)) { // OOM: draw only the pages uploaded so far
                ctx->pageCount = pageIdx;
                ctx->pieceCount = pg->firstPiece;
                break;
            }
            ASS_OVERLAY_PROF(long long tB = assOverlayNowUs();)
            assOverlayFillPage(ctx, pg, ctx->atlasBuf);
            ASS_OVERLAY_PROF(
                long long tC = assOverlayNowUs();
                ctx->profFillUs += tC - tB;
            )
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pg->usedW, pg->usedH, format, GL_UNSIGNED_BYTE, ctx->atlasBuf);
            ASS_OVERLAY_PROF(ctx->profTexUs += assOverlayNowUs() - tC;)
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

// changed == 1 from libass means only positions moved: the exact same bitmaps in the same order.
// The uploaded atlas is then still valid as-is, so only the pieces' destinations need refreshing -
// no pack, no upload. Returns false if the list doesn't line up with the current pieces (e.g. a piece
// was dropped or truncated by OOM last time), in which case the caller does a full repack instead.
static bool assOverlayUpdatePositions(AssOverlayContext* ctx, ASS_Image* image) {
    int pieceIdx = 0;
    for (ASS_Image* img = image; img != NULL; img = img->next) {
        if (assOverlayIsEmpty(img)) continue;
        if (pieceIdx >= ctx->pieceCount) return false;
        AssOverlayPiece* p = &ctx->pieces[pieceIdx];
        if (p->bitmap != img->bitmap || p->w != img->w || p->h != img->h) return false;
        p->dstX = img->dst_x;
        p->dstY = img->dst_y;
        pieceIdx++;
    }
    return pieceIdx == ctx->pieceCount;
}

// Rebuilds the vertex buffer from ctx->pieces[] (positions + current page UV normalization).
static void assOverlayBuildVertices(AssOverlayContext* ctx) {
    // vertexBuf is sized together with pieces[] (see assOverlayEnsurePieces), so it always fits.
    int needed = ctx->pieceCount * ASS_OVERLAY_VERTICES_PER_PIECE;

    const float sx = 2.0f / (float) ctx->frameW;
    const float sy = 2.0f / (float) ctx->frameH;
    AssOverlayVertex* out = ctx->vertexBuf;
    for (int pageIdx = 0; pageIdx < ctx->pageCount; pageIdx++) {
        const AssOverlayPage* pg = &ctx->pages[pageIdx];
        const float su = 1.0f / (float) pg->capW;
        const float sv = 1.0f / (float) pg->capH;
        for (int pieceIdx = pg->firstPiece; pieceIdx < pg->firstPiece + pg->pieceCount; pieceIdx++) {
            const AssOverlayPiece* p = &ctx->pieces[pieceIdx];
            // Clip-space Y-flip: libass y is top-down, GL clip y is bottom-up.
            float x0 = (float) p->dstX * sx - 1.0f;
            float x1 = (float) (p->dstX + p->w) * sx - 1.0f;
            float y0 = 1.0f - (float) p->dstY * sy;
            float y1 = 1.0f - (float) (p->dstY + p->h) * sy;

            // Exact piece rect: the quad covers w x h pixels and samples exactly w x h texels, so
            // each fragment hits a texel center.
            float u0 = (float) p->atlasX * su;
            float u1 = (float) (p->atlasX + p->w) * su;
            float v0 = (float) p->atlasY * sv;
            float v1 = (float) (p->atlasY + p->h) * sv;

            uint8_t r, g, b, a;
            assOverlayUnpackColor(p->color, &r, &g, &b, &a);

            // Two triangles, emitted in pieces[] (libass list) order, which is the paint order -
            // independent of where the packer placed the piece in the atlas.
            out[0] = (AssOverlayVertex) {x0, y0, u0, v0, r, g, b, a};
            out[1] = (AssOverlayVertex) {x1, y0, u1, v0, r, g, b, a};
            out[2] = (AssOverlayVertex) {x0, y1, u0, v1, r, g, b, a};
            out[3] = out[1];
            out[4] = (AssOverlayVertex) {x1, y1, u1, v1, r, g, b, a};
            out[5] = out[2];
            out += ASS_OVERLAY_VERTICES_PER_PIECE;
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, ctx->atlasVbo);
    // Storage is only (re)specified when it must grow, to the full vertexBuf capacity; otherwise
    // it is updated in place. Re-specifying it on every change (orphaning) would hand out fresh
    // storage each time, which on ANGLE is a new allocation. The previous draw may still be reading
    // it, but glBufferSubData is defined to not affect commands already issued: the driver handles
    // that hazard, cheaply for a buffer this small.
    if (ctx->vboCap < ctx->vertexCap) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) ((size_t) ctx->vertexCap * sizeof(AssOverlayVertex)), NULL, GL_DYNAMIC_DRAW);
        ctx->vboCap = ctx->vertexCap;
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr) ((size_t) needed * sizeof(AssOverlayVertex)), ctx->vertexBuf);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// Brings the atlas pages and vertex buffer in line with libass's latest output. With changed == 0
// the previous frame's state is reused untouched.
static void assOverlayUpdate(AssOverlayContext* ctx, ASS_Image* image, int changed) {
    ASS_OVERLAY_PROF(
        ctx->profPackUs = ctx->profTexAllocUs = ctx->profMapUs = ctx->profFillUs = ctx->profTexUs = ctx->profVertUs = 0;
        ctx->profPieceArea = ctx->profUsedArea = ctx->profCapArea = 0;
    )
    if (changed == 1 && ctx->pieceCount > 0 && assOverlayUpdatePositions(ctx, image)) {
        // Positions only: atlas pages are still valid, just move the quads.
        ASS_OVERLAY_PROF(long long t0 = assOverlayNowUs();)
        assOverlayBuildVertices(ctx);
        ASS_OVERLAY_PROF(ctx->profVertUs = assOverlayNowUs() - t0;)
    } else if (changed != 0) {
        ASS_OVERLAY_PROF(long long t0 = assOverlayNowUs();)
        assOverlayPack(ctx, image);
        ASS_OVERLAY_PROF(ctx->profPackUs = assOverlayNowUs() - t0;)
        if (ctx->pieceCount > 0) {
            assOverlayUpload(ctx);
            ASS_OVERLAY_PROF(long long t1 = assOverlayNowUs();)
            assOverlayBuildVertices(ctx);
            ASS_OVERLAY_PROF(ctx->profVertUs = assOverlayNowUs() - t1;)
        }
    }
}

// Composites every page onto `fbo` with one draw call per page, then restores the GL state.
static void assOverlayDrawPages(const AssOverlayContext* ctx, GLuint fbo) {
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, ctx->frameW, ctx->frameH);

    glEnable(GL_BLEND);
    // The fragment shader outputs premultiplied color (opacity * coverage): standard premultiplied "over".
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(ctx->atlasProgram);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->atlasVbo);
    const GLsizei stride = sizeof(AssOverlayVertex);
    glEnableVertexAttribArray((GLuint) ctx->atlasAPosition);
    glVertexAttribPointer((GLuint) ctx->atlasAPosition, 2, GL_FLOAT, GL_FALSE, stride, (void*) offsetof(AssOverlayVertex, x));
    glEnableVertexAttribArray((GLuint) ctx->atlasATexCoord);
    glVertexAttribPointer((GLuint) ctx->atlasATexCoord, 2, GL_FLOAT, GL_FALSE, stride, (void*) offsetof(AssOverlayVertex, u));
    glEnableVertexAttribArray((GLuint) ctx->atlasAColor);
    glVertexAttribPointer((GLuint) ctx->atlasAColor, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*) offsetof(AssOverlayVertex, r));
    glActiveTexture(GL_TEXTURE0);
    // One draw per page, in page order: each page is a contiguous run of the libass list, so this
    // paints every piece in exactly libass's order.
    for (int pageIdx = 0; pageIdx < ctx->pageCount; pageIdx++) {
        const AssOverlayPage* pg = &ctx->pages[pageIdx];
        glBindTexture(GL_TEXTURE_2D, pg->tex);
        glDrawArrays(GL_TRIANGLES, pg->firstPiece * ASS_OVERLAY_VERTICES_PER_PIECE,
            pg->pieceCount * ASS_OVERLAY_VERTICES_PER_PIECE);
    }

    // Leave GL state the way media3's own shader programs expect it.
    glDisable(GL_BLEND);
    glDisableVertexAttribArray((GLuint) ctx->atlasAPosition);
    glDisableVertexAttribArray((GLuint) ctx->atlasATexCoord);
    glDisableVertexAttribArray((GLuint) ctx->atlasAColor);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prevFbo);
}

// Called every video frame, on the GL thread, from AssRender.kt's drawOverlayFrame (itself called
// from AssGlShaderProgram.queueInputFrame). `fbo` wraps the frame's own decoded-video texture -
// this draws directly into it, in place; the caller forwards that same texture downstream
// unmodified, so anything this function doesn't touch (i.e. every frame with no visible subtitle
// content) costs nothing beyond the ass_render_frame call itself.
jlong nativeAssOverlayDraw(JNIEnv* env, jclass clazz, jlong overlay, jlong render, jlong track,
    jint fbo, jint frameWidth, jint frameHeight, jlong timeMs) {
    if (render == 0 || track == 0) return overlay;
    AssOverlayContext* ctx = (AssOverlayContext*) overlay;
    if (ctx == NULL) {
        ctx = assOverlayCreate();
        if (ctx == NULL) return 0;
    }

    if (frameWidth != ctx->frameW || frameHeight != ctx->frameH) {
        ctx->frameW = frameWidth;
        ctx->frameH = frameHeight;
        // libass always renders at the frame's own size, so storage and frame size are the same.
        ass_set_storage_size((ASS_Renderer*) render, frameWidth, frameHeight);
        ass_set_frame_size((ASS_Renderer*) render, frameWidth, frameHeight);
        LOGI("ass_set_storage_size/ass_set_frame_size(%d, %d)", frameWidth, frameHeight);
    }

    long long t0 = assOverlayNowMs();
    int changed = 0;
    ASS_Image* image = ass_render_frame((ASS_Renderer*) render, (ASS_Track*) track, timeMs, &changed);
    long long t1 = assOverlayNowMs();
    assOverlayUpdate(ctx, image, changed);
    long long t2 = assOverlayNowMs();
    if (t2 - t0 >= ASS_OVERLAY_SLOW_LOG_MS) {
        LOGW("timeMs=%lld (changed=%d): ass_render_frame %lldms, atlas update %lldms (pieces=%d, pages=%d)",
            (long long) timeMs, changed, t1 - t0, t2 - t1, ctx->pieceCount, ctx->pageCount);
#ifdef ASS_OVERLAY_PROFILE
        LOGW("  breakdown us: pack=%lld texAlloc=%lld map=%lld fill=%lld unmap+texSubImage=%lld vertices=%lld | "
             "area: pieces=%lld used=%lld cap=%lld (page0 %dx%d used, %dx%d cap)",
            ctx->profPackUs, ctx->profTexAllocUs, ctx->profMapUs, ctx->profFillUs, ctx->profTexUs, ctx->profVertUs,
            ctx->profPieceArea, ctx->profUsedArea, ctx->profCapArea,
            ctx->pageCount > 0 ? ctx->pages[0].usedW : 0, ctx->pageCount > 0 ? ctx->pages[0].usedH : 0,
            ctx->pageCount > 0 ? ctx->pages[0].capW : 0, ctx->pageCount > 0 ? ctx->pages[0].capH : 0);
#endif
    }

    // pieceCount is sticky across changed==0 frames (same visible content as last time): still
    // needs to be drawn every single call, since each call's `fbo` wraps a DIFFERENT decoded video
    // frame that doesn't have the subtitle on it yet.
    if (ctx->pieceCount > 0) {
        assOverlayDrawPages(ctx, (GLuint) fbo);
    }
    return (jlong) ctx;
}

void nativeAssOverlayRelease(JNIEnv* env, jclass clazz, jlong overlay) {
    assOverlayDestroy((AssOverlayContext*) overlay);
}
