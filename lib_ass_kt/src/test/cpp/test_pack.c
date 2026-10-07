// Tests the atlas layout (AssAtlas.c) on recorded frames (see frame_dump.h): no GPU involved.
//
// usage: test_pack <max-atlas-size>[,<max-atlas-size>...] <dump>...
//
// Every frame is fed to assAtlasPack, once per page size limit, as an ASS_Image list whose bitmaps
// hold pseudo-random pixels: one buffer per distinct bitmap, so repeats share a pointer as they do
// with libass, with padded strides. Then: every piece that fits in a page must be kept, in list
// order, with its fields (bigger ones are dropped by design); pages must be contiguous runs of the
// list within the limit; after assAtlasFillPage, every piece's atlas rect must hold exactly its own
// bitmap, and distinct bitmaps must never overlap; and when nothing was dropped, assAtlasMovePieces
// must accept the same list. Exits with 1 on the first failure.
//
// Prints, per dump and limit, the upload per frame (the pages' packed areas, in MB of R8 texels and
// relative to the distinct bitmap texels) and the packing time.
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "AssAtlas.h"
#include "frame_dump.h"

#define MAX_LIMITS 16

typedef struct {
    AssAtlas atlas;
    long long distinctTexels, uploadTexels, packNs, maxPackNs;
    int frames, maxPages, multiPageFrames, dropped;
} Limit;

static const char* gDump;
static int gFrame;
static int gMaxSize;

static void fail(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "FAIL %s frame %d, pages up to %d: ", gDump, gFrame, gMaxSize);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
    exit(1);
}

static long long nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static uint64_t splitMix64(uint64_t* state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// A bitmap of pseudo-random pixels, different for every (frame, uid): a misplaced, swapped or
// truncated piece can't go unnoticed. The stride padding holds a constant.
static unsigned char* makeBitmap(int w, int h, int stride, uint64_t seed) {
    unsigned char* bitmap = (unsigned char*) malloc((size_t) stride * h);
    if (bitmap == NULL) fail("out of memory");
    uint64_t state = seed;
    for (int y = 0; y < h; y++) {
        unsigned char* row = bitmap + (size_t) y * stride;
        for (int x = 0; x < w; x += 8) {
            const uint64_t random = splitMix64(&state);
            memcpy(row + x, &random, (size_t) (w - x < 8 ? w - x : 8));
        }
        memset(row + w, 0xEE, (size_t) (stride - w));
    }
    return bitmap;
}

static void checkPage(const AssAtlas* atlas, const AssAtlasPage* page, int pageIdx) {
    unsigned char* buf = (unsigned char*) malloc((size_t) page->usedW * page->usedH);
    if (buf == NULL) fail("out of memory");
    memset(buf, 0xA5, (size_t) page->usedW * page->usedH); // poison: gaps are never sampled
    assAtlasFillPage(atlas, page, buf);
    for (int i = page->firstPiece; i < page->firstPiece + page->pieceCount; i++) {
        const AssAtlasPiece* p = &atlas->pieces[i];
        if (p->atlasX < 0 || p->atlasY < 0 || p->atlasX + p->w > page->usedW || p->atlasY + p->h > page->usedH) {
            fail("piece %d (%dx%d at %d,%d) is outside page %d (%dx%d)", i, p->w, p->h, p->atlasX, p->atlasY, pageIdx, page->usedW, page->usedH);
        }
        for (int row = 0; row < p->h; row++) {
            if (memcmp(buf + (size_t) (p->atlasY + row) * page->usedW + p->atlasX, p->bitmap + (size_t) row * p->stride, (size_t) p->w) != 0) {
                fail("piece %d (%dx%d at %d,%d on page %d): row %d doesn't hold its bitmap", i, p->w, p->h, p->atlasX, p->atlasY, pageIdx, row);
            }
        }
        for (int j = page->firstPiece; j < i; j++) {
            const AssAtlasPiece* q = &atlas->pieces[j];
            const bool overlap = p->atlasX < q->atlasX + q->w && q->atlasX < p->atlasX + p->w &&
                p->atlasY < q->atlasY + q->h && q->atlasY < p->atlasY + p->h;
            const bool sameBitmapSameSpot = p->bitmap == q->bitmap && p->w == q->w && p->h == q->h &&
                p->stride == q->stride && p->atlasX == q->atlasX && p->atlasY == q->atlasY;
            if (overlap && !sameBitmapSameSpot) fail("pieces %d and %d overlap on page %d", j, i, pageIdx);
        }
    }
    free(buf);
}

static void testFrame(Limit* limit, const DumpFrame* frame, const ASS_Image* images) {
    AssAtlas* atlas = &limit->atlas;
    gMaxSize = atlas->maxPageSize;
    const long long start = nowNs();
    assAtlasPack(atlas, images);
    const long long elapsed = nowNs() - start;

    // Every piece that fits in a page is kept, in order, as is.
    int kept = 0, dropped = 0;
    for (int i = 0, nextUid = 0; i < frame->count; i++) {
        const ASS_Image* img = &images[i];
        const bool firstCopy = frame->pieces[i].uid == nextUid; // uids number bitmaps in order of appearance
        if (firstCopy) nextUid++;
        if (img->w > atlas->maxPageSize || img->h > atlas->maxPageSize) {
            dropped++;
            continue;
        }
        if (firstCopy) limit->distinctTexels += (long long) img->w * img->h;
        if (kept >= atlas->pieceCount) fail("only %d of the pieces that fit were kept", atlas->pieceCount);
        const AssAtlasPiece* p = &atlas->pieces[kept++];
        if (p->bitmap != img->bitmap || p->w != img->w || p->h != img->h || p->stride != img->stride ||
            p->dstX != img->dst_x || p->dstY != img->dst_y || p->color != img->color) {
            fail("piece %d doesn't match image %d", kept - 1, i);
        }
    }
    if (kept != atlas->pieceCount) fail("%d pieces kept, %d fit", atlas->pieceCount, kept);

    int nextPiece = 0;
    for (int pageIdx = 0; pageIdx < atlas->pageCount; pageIdx++) {
        const AssAtlasPage* page = &atlas->pages[pageIdx];
        if (page->firstPiece != nextPiece || page->pieceCount <= 0) fail("page %d isn't the next run of the list", pageIdx);
        if (page->usedW < 1 || page->usedH < 1 || page->usedW > atlas->maxPageSize || page->usedH > atlas->maxPageSize) {
            fail("page %d is %dx%d", pageIdx, page->usedW, page->usedH);
        }
        nextPiece += page->pieceCount;
        limit->uploadTexels += (long long) page->usedW * page->usedH;
        checkPage(atlas, page, pageIdx);
    }
    if (nextPiece != atlas->pieceCount) fail("pages cover %d of %d pieces", nextPiece, atlas->pieceCount);
    if (dropped == 0 && !assAtlasMovePieces(atlas, images)) fail("assAtlasMovePieces rejected the same list");

    limit->frames++;
    limit->dropped += dropped;
    limit->packNs += elapsed;
    if (elapsed > limit->maxPackNs) limit->maxPackNs = elapsed;
    if (atlas->pageCount > limit->maxPages) limit->maxPages = atlas->pageCount;
    if (atlas->pageCount > 1) limit->multiPageFrames++;
}

int main(int argc, char** argv) {
    int maxSizes[MAX_LIMITS];
    int limitCount = 0;
    bool validArgs = argc >= 3;
    for (char* size = validArgs ? strtok(argv[1], ",") : NULL; size != NULL; size = strtok(NULL, ",")) {
        if (limitCount == MAX_LIMITS || atoi(size) <= 0) {
            validArgs = false;
            break;
        }
        maxSizes[limitCount++] = atoi(size);
    }
    if (!validArgs || limitCount == 0) {
        fprintf(stderr, "usage: %s <max-atlas-size>[,<max-atlas-size>...] <dump>...\n", argv[0]);
        return 2;
    }

    Limit limits[MAX_LIMITS];
    for (int dumpIdx = 2; dumpIdx < argc; dumpIdx++) {
        gDump = frameDumpName(argv[dumpIdx]);
        FrameDump dump = loadFrameDump(argv[dumpIdx]);
        for (int l = 0; l < limitCount; l++) limits[l] = (Limit) {.atlas = {.maxPageSize = maxSizes[l]}};

        for (gFrame = 0; gFrame < dump.frameCount; gFrame++) {
            const DumpFrame* frame = &dump.frames[gFrame];
            if (frame->count == 0) continue;
            unsigned char** bitmaps = (unsigned char**) calloc((size_t) frame->uniqueCount, sizeof(unsigned char*));
            ASS_Image* images = (ASS_Image*) calloc((size_t) frame->count, sizeof(ASS_Image));
            if (bitmaps == NULL || images == NULL) fail("out of memory");
            for (int i = 0; i < frame->count; i++) {
                const DumpPiece* piece = &frame->pieces[i];
                const int stride = piece->w + (piece->uid % 3) * 7;
                if (bitmaps[piece->uid] == NULL) {
                    bitmaps[piece->uid] = makeBitmap(piece->w, piece->h, stride, (uint64_t) gFrame << 32 | (uint32_t) piece->uid);
                }
                images[i] = (ASS_Image) {.w = piece->w, .h = piece->h, .stride = stride, .bitmap = bitmaps[piece->uid],
                    .color = 0x10203000u + (uint32_t) i, .dst_x = piece->dstX, .dst_y = piece->dstY,
                    .next = i + 1 < frame->count ? &images[i + 1] : NULL};
            }
            for (int l = 0; l < limitCount; l++) testFrame(&limits[l], frame, images);
            for (int uid = 0; uid < frame->uniqueCount; uid++) free(bitmaps[uid]);
            free(bitmaps);
            free(images);
        }

        for (int l = 0; l < limitCount; l++) {
            Limit* limit = &limits[l];
            assAtlasFree(&limit->atlas);
            if (limit->frames == 0) {
                printf("%-34s max %5d: no frames with subtitles\n", gDump, maxSizes[l]);
                continue;
            }
            printf("%-34s max %5d: %4d frames, upload %6.2f MB/frame = %5.1f%% of distinct texels, up to %d pages "
                   "(%d multi-page frames), pack %4.0f us avg, %5.0f us max",
                gDump, maxSizes[l], limit->frames, limit->uploadTexels / 1e6 / limit->frames,
                limit->distinctTexels > 0 ? 100.0 * limit->uploadTexels / limit->distinctTexels : 0.0,
                limit->maxPages, limit->multiPageFrames, limit->packNs / 1e3 / limit->frames, limit->maxPackNs / 1e3);
            if (limit->dropped > 0) printf(", %d pieces larger than a page dropped", limit->dropped);
            printf("\n");
        }
        freeFrameDump(&dump);
    }
    return 0;
}
