// Compares atlas packing strategies on recorded frames (see frame_dump.h).
//
// usage: bench_packers <max-atlas-size> <dump>...
//
// The first row is the overlay's own packer (assAtlasPack). The others re-implement its page
// driver - every page holds the longest run of the list that fits, at a power-of-two width, up to
// the max atlas size in height - around variations of the page packer, of the order pieces are
// packed in, of sharing repeated bitmaps, and of how a run that doesn't fit is shortened. The second
// row is the re-implementation of assAtlasPack itself, so it should match the first.
//
// For each, prints the atlas upload per frame (the pages' used areas, in MB of R8 texels, and
// relative to the frame's distinct bitmap texels), the worst frame, pages, and packing time. Every
// layout is checked for bounds and overlaps.
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "AssAtlas.h"
#include "frame_dump.h"

typedef enum { PACKER_SKYLINE_BL, PACKER_SKYLINE_BF, PACKER_SHELF, PACKER_MAXRECTS } Packer;
typedef enum { ORDER_TALLEST, ORDER_LARGEST, ORDER_WIDEST, ORDER_LONGEST_SIDE } Order;
typedef enum { SPLIT_SHRINK, SPLIT_BISECT } Split;

typedef struct {
    const char* name;
    bool assAtlasPack; // the real thing; the other fields don't apply
    Packer packer;
    int orderCount;
    Order orders[3]; // with several, each page keeps the order that packs it smallest
    bool shareRepeats;
    Split split;
} Variant;

static const Variant kVariants[] = {
    {.name = "AssAtlas.c (assAtlasPack)", .assAtlasPack = true},
    {"skyline BL, tallest first", false, PACKER_SKYLINE_BL, 1, {ORDER_TALLEST}, true, SPLIT_SHRINK},
    {"  no repeat sharing", false, PACKER_SKYLINE_BL, 1, {ORDER_TALLEST}, false, SPLIT_SHRINK},
    {"  bisected page splits", false, PACKER_SKYLINE_BL, 1, {ORDER_TALLEST}, true, SPLIT_BISECT},
    {"skyline BF (stb), tallest", false, PACKER_SKYLINE_BF, 1, {ORDER_TALLEST}, true, SPLIT_SHRINK},
    {"skyline BL, largest first", false, PACKER_SKYLINE_BL, 1, {ORDER_LARGEST}, true, SPLIT_SHRINK},
    {"skyline BL, widest first", false, PACKER_SKYLINE_BL, 1, {ORDER_WIDEST}, true, SPLIT_SHRINK},
    {"skyline BL, longest side", false, PACKER_SKYLINE_BL, 1, {ORDER_LONGEST_SIDE}, true, SPLIT_SHRINK},
    {"skyline BL, best of 3 orders", false, PACKER_SKYLINE_BL, 3, {ORDER_TALLEST, ORDER_LARGEST, ORDER_WIDEST}, true, SPLIT_SHRINK},
    {"shelf first-fit, tallest", false, PACKER_SHELF, 1, {ORDER_TALLEST}, true, SPLIT_SHRINK},
    {"maxrects BL, largest first", false, PACKER_MAXRECTS, 1, {ORDER_LARGEST}, true, SPLIT_SHRINK},
};

typedef struct { int x, y; } Node;
typedef struct { int x, y, w, h; } Rect;
typedef struct { long long key1, key2; int piece; } OrderEntry;
typedef struct { int first, count, usedW, usedH; } Page;

// Scratch for one dump, sized for its largest frame.
typedef struct {
    const DumpPiece* pieces;
    int count, maxSize;
    int* sameAs; // previous piece showing the same bitmap, or -1
    int* lastOfUid;
    int* x;
    int* y;
    int* tryX; // the attempt in progress
    int* tryY;
    int* run; // distinct pieces of the page being packed
    OrderEntry* order;
    Node* nodes; // skyline: node i covers [nodes[i].x, nodes[i + 1].x) up to nodes[i].y; nodes[count] ends it
    Rect* shelves; // x = filled width, y = top, h = height
    Rect* freeRects; // maxrects
    int freeCount, freeCap;
    Page* pages;
    int pageCount;
} Bench;

static void* allocOrDie(size_t bytes) {
    void* p = malloc(bytes > 0 ? bytes : 1);
    if (p == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    return p;
}

static long long nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// AssAtlas.c's page width rule.
static int pageWidthFor(long long area, int widest, int maxSize) {
    int width = 256;
    while ((long long) width * width < area && width < maxSize) width *= 2;
    while (width < widest && width < maxSize) width *= 2;
    return width < maxSize ? width : maxSize;
}

static int compareEntries(const void* a, const void* b) {
    const OrderEntry* p = (const OrderEntry*) a;
    const OrderEntry* q = (const OrderEntry*) b;
    if (p->key1 != q->key1) return p->key1 < q->key1 ? 1 : -1;
    if (p->key2 != q->key2) return p->key2 < q->key2 ? 1 : -1;
    return p->piece - q->piece;
}

static void sortRun(Bench* b, int runCount, Order order) {
    for (int k = 0; k < runCount; k++) {
        const DumpPiece* p = &b->pieces[b->run[k]];
        OrderEntry* e = &b->order[k];
        e->piece = b->run[k];
        switch (order) {
        case ORDER_TALLEST: e->key1 = p->h; e->key2 = p->w; break;
        case ORDER_LARGEST: e->key1 = (long long) p->w * p->h; e->key2 = p->h; break;
        case ORDER_WIDEST: e->key1 = p->w; e->key2 = p->h; break;
        case ORDER_LONGEST_SIDE:
            e->key1 = p->w > p->h ? p->w : p->h;
            e->key2 = p->w > p->h ? p->h : p->w;
            break;
        }
    }
    qsort(b->order, (size_t) runCount, sizeof(OrderEntry), compareEntries);
}

// --- Skyline --------------------------------------------------------------------------------------

// Bottom-left (AssSkyline.c's rule): lowest, then leftmost, with the left edge on a node.
static bool skylineFindLowest(const Node* nodes, int count, int pageW, int maxH, int w, int h, int* outX, int* outY) {
    int bestY = maxH - h + 1, bestX = -1;
    for (int i = 0; i < count && nodes[i].x + w <= pageW; i++) {
        int y = 0;
        for (int k = i; nodes[k].x < nodes[i].x + w && y < bestY; k++) {
            if (nodes[k].y > y) y = nodes[k].y;
        }
        if (y < bestY) {
            bestY = y;
            bestX = nodes[i].x;
        }
    }
    *outX = bestX;
    *outY = bestY;
    return bestX >= 0;
}

// Height a w-wide piece rests at with its left edge at x (inside node `node`), and the area left
// empty under it.
static int skylineRest(const Node* nodes, int node, int x, int w, long long* waste) {
    int y = 0;
    for (int k = node; nodes[k].x < x + w; k++) {
        if (nodes[k].y > y) y = nodes[k].y;
    }
    long long wasted = 0;
    for (int k = node; nodes[k].x < x + w; k++) {
        const int left = nodes[k].x > x ? nodes[k].x : x;
        const int right = nodes[k + 1].x < x + w ? nodes[k + 1].x : x + w;
        wasted += (long long) (right - left) * (y - nodes[k].y);
    }
    *waste = wasted;
    return y;
}

// Best-fit (stb_rect_pack's rule): also tries the right edge on every node's end, and breaks ties
// in height on the least area left empty under the piece.
static bool skylineFindBestFit(const Node* nodes, int count, int pageW, int maxH, int w, int h, int* outX, int* outY) {
    int bestX = -1, bestY = INT_MAX;
    long long bestWaste = LLONG_MAX;
    for (int i = 0; i < count && nodes[i].x + w <= pageW; i++) {
        long long waste;
        const int y = skylineRest(nodes, i, nodes[i].x, w, &waste);
        if (y + h > maxH) continue;
        if (y < bestY || (y == bestY && waste < bestWaste)) {
            bestX = nodes[i].x;
            bestY = y;
            bestWaste = waste;
        }
    }
    for (int i = 0, node = 0; i < count; i++) {
        const int x = nodes[i + 1].x - w;
        if (x < 0) continue;
        while (nodes[node + 1].x <= x) node++;
        long long waste;
        const int y = skylineRest(nodes, node, x, w, &waste);
        if (y + h > maxH) continue;
        if (y < bestY || (y == bestY && (waste < bestWaste || (waste == bestWaste && x < bestX)))) {
            bestX = x;
            bestY = y;
            bestWaste = waste;
        }
    }
    *outX = bestX;
    *outY = bestY;
    return bestX >= 0;
}

// Raises the columns [x, x + w) to `top`, keeping the uncovered parts of the nodes at both ends.
static void skylinePlace(Node* nodes, int* count, int x, int w, int top) {
    int first = 0;
    while (nodes[first + 1].x <= x) first++;
    int end = first + 1;
    while (nodes[end].x < x + w) end++;
    Node replacement[3];
    int n = 0;
    if (nodes[first].x < x) replacement[n++] = nodes[first];
    replacement[n++] = (Node) {x, top};
    if (nodes[end].x > x + w) replacement[n++] = (Node) {x + w, nodes[end - 1].y};
    memmove(&nodes[first + n], &nodes[end], (size_t) (*count + 1 - end) * sizeof(Node));
    memcpy(&nodes[first], replacement, (size_t) n * sizeof(Node));
    *count += n - (end - first);
    // Merge same-height neighbors around the change (never the end marker).
    const int last = first + n < *count - 1 ? first + n : *count - 1;
    for (int i = last; i >= first && i >= 1; i--) {
        if (nodes[i].y != nodes[i - 1].y) continue;
        memmove(&nodes[i], &nodes[i + 1], (size_t) (*count - i) * sizeof(Node));
        (*count)--;
    }
}

// --- MaxRects -------------------------------------------------------------------------------------

static void maxrectsAddFree(Bench* b, Rect r) {
    if (r.w <= 0 || r.h <= 0) return;
    if (b->freeCount == b->freeCap) {
        b->freeCap = b->freeCap > 0 ? b->freeCap * 2 : 256;
        b->freeRects = (Rect*) realloc(b->freeRects, (size_t) b->freeCap * sizeof(Rect));
        if (b->freeRects == NULL) {
            fprintf(stderr, "out of memory\n");
            exit(2);
        }
    }
    b->freeRects[b->freeCount++] = r;
}

// Bottom-left: the free rect where the piece's top ends lowest, then leftmost.
static bool maxrectsFind(const Bench* b, int w, int h, int* outX, int* outY) {
    int bestTop = INT_MAX, bestX = INT_MAX;
    for (int i = 0; i < b->freeCount; i++) {
        const Rect* r = &b->freeRects[i];
        if (r->w < w || r->h < h) continue;
        if (r->y + h < bestTop || (r->y + h == bestTop && r->x < bestX)) {
            bestTop = r->y + h;
            bestX = r->x;
            *outY = r->y;
        }
    }
    *outX = bestX;
    return bestTop != INT_MAX;
}

static void maxrectsPlace(Bench* b, int x, int y, int w, int h) {
    const int before = b->freeCount;
    for (int i = 0; i < before; i++) {
        const Rect r = b->freeRects[i];
        if (x >= r.x + r.w || x + w <= r.x || y >= r.y + r.h || y + h <= r.y) continue;
        maxrectsAddFree(b, (Rect) {r.x, r.y, x - r.x, r.h});
        maxrectsAddFree(b, (Rect) {x + w, r.y, r.x + r.w - x - w, r.h});
        maxrectsAddFree(b, (Rect) {r.x, r.y, r.w, y - r.y});
        maxrectsAddFree(b, (Rect) {r.x, y + h, r.w, r.y + r.h - y - h});
        b->freeRects[i].w = 0; // split: removed below
    }
    // Drop split rects, and rects contained in another.
    for (int i = 0; i < b->freeCount; i++) {
        const Rect a = b->freeRects[i];
        for (int j = 0; j < b->freeCount && a.w > 0; j++) {
            const Rect c = b->freeRects[j];
            if (i != j && c.w > 0 && a.x >= c.x && a.y >= c.y && a.x + a.w <= c.x + c.w && a.y + a.h <= c.y + c.h) {
                b->freeRects[i].w = 0;
            }
        }
    }
    int kept = 0;
    for (int i = 0; i < b->freeCount; i++) {
        if (b->freeRects[i].w > 0) b->freeRects[kept++] = b->freeRects[i];
    }
    b->freeCount = kept;
}

// --- Pages ----------------------------------------------------------------------------------------

// Packs b->order[0, runCount) into a pageW wide page, up to the max atlas size in height.
static bool packRun(Bench* b, Packer packer, int runCount, int pageW, int* usedW, int* usedH) {
    const int maxH = b->maxSize;
    int nodeCount = 1, shelfCount = 0, shelfTop = 0;
    b->nodes[0] = (Node) {0, 0};
    b->nodes[1] = (Node) {pageW, 0};
    b->freeCount = 0;
    if (packer == PACKER_MAXRECTS) maxrectsAddFree(b, (Rect) {0, 0, pageW, maxH});
    *usedW = 0;
    *usedH = 0;
    for (int k = 0; k < runCount; k++) {
        const int piece = b->order[k].piece;
        const int w = b->pieces[piece].w, h = b->pieces[piece].h;
        int x = 0, y = 0;
        switch (packer) {
        case PACKER_SKYLINE_BL:
        case PACKER_SKYLINE_BF: {
            const bool found = packer == PACKER_SKYLINE_BL
                ? skylineFindLowest(b->nodes, nodeCount, pageW, maxH, w, h, &x, &y)
                : skylineFindBestFit(b->nodes, nodeCount, pageW, maxH, w, h, &x, &y);
            if (!found) return false;
            skylinePlace(b->nodes, &nodeCount, x, w, y + h);
            break;
        }
        case PACKER_SHELF: {
            int s = 0;
            while (s < shelfCount && (b->shelves[s].x + w > pageW || h > b->shelves[s].h)) s++;
            if (s == shelfCount) {
                if (shelfTop + h > maxH) return false;
                b->shelves[shelfCount++] = (Rect) {0, shelfTop, 0, h};
                shelfTop += h;
            }
            x = b->shelves[s].x;
            y = b->shelves[s].y;
            b->shelves[s].x += w;
            break;
        }
        case PACKER_MAXRECTS:
            if (!maxrectsFind(b, w, h, &x, &y)) return false;
            maxrectsPlace(b, x, y, w, h);
            break;
        }
        b->tryX[piece] = x;
        b->tryY[piece] = y;
        if (x + w > *usedW) *usedW = x + w;
        if (y + h > *usedH) *usedH = y + h;
    }
    return true;
}

static long long runTexels(const Bench* b, int first, int piece) {
    return b->sameAs[piece] >= first ? 0 : (long long) b->pieces[piece].w * b->pieces[piece].h;
}

static bool packPage(Bench* b, const Variant* v, int first, int end, Page* page) {
    int runCount = 0, widest = 0;
    long long area = 0;
    for (int i = first; i < end; i++) {
        if (b->sameAs[i] >= first) continue;
        b->run[runCount++] = i;
        area += runTexels(b, first, i);
        if (b->pieces[i].w > widest) widest = b->pieces[i].w;
    }
    const int pageW = pageWidthFor(area, widest, b->maxSize);
    long long bestArea = -1;
    for (int o = 0; o < v->orderCount; o++) {
        sortRun(b, runCount, v->orders[o]);
        int usedW, usedH;
        if (!packRun(b, v->packer, runCount, pageW, &usedW, &usedH)) continue;
        if (bestArea >= 0 && (long long) usedW * usedH >= bestArea) continue;
        bestArea = (long long) usedW * usedH;
        *page = (Page) {first, end - first, usedW, usedH};
        for (int k = 0; k < runCount; k++) {
            b->x[b->run[k]] = b->tryX[b->run[k]];
            b->y[b->run[k]] = b->tryY[b->run[k]];
        }
    }
    if (bestArea < 0) return false;
    for (int i = first; i < end; i++) {
        if (b->sameAs[i] < first) continue;
        b->x[i] = b->x[b->sameAs[i]];
        b->y[i] = b->y[b->sameAs[i]];
    }
    return true;
}

static void packFrame(Bench* b, const Variant* v, const DumpFrame* frame) {
    for (int uid = 0; uid < frame->uniqueCount; uid++) b->lastOfUid[uid] = -1;
    for (int i = 0; i < b->count; i++) {
        const int uid = b->pieces[i].uid;
        b->sameAs[i] = v->shareRepeats ? b->lastOfUid[uid] : -1;
        b->lastOfUid[uid] = i;
    }
    const long long pageArea = (long long) b->maxSize * b->maxSize;
    b->pageCount = 0;
    for (int first = 0; first < b->count;) {
        long long runArea = runTexels(b, first, first);
        int end = first + 1;
        while (end < b->count && runArea + runTexels(b, first, end) <= pageArea) runArea += runTexels(b, first, end++);
        Page page;
        if (!packPage(b, v, first, end, &page)) {
            if (v->split == SPLIT_SHRINK) {
                // AssAtlas.c's rule: drop the run's last 1/8 of texels (and at least one piece).
                do {
                    const long long target = runArea - runArea / 8;
                    runArea = runTexels(b, first, first);
                    int shorter = first + 1;
                    while (shorter < end - 1 && runArea + runTexels(b, first, shorter) <= target) runArea += runTexels(b, first, shorter++);
                    end = shorter;
                } while (!packPage(b, v, first, end, &page));
            } else {
                int fits = first + 1, fails = end; // a lone piece always fits
                while (fails - fits > 1) {
                    const int mid = fits + (fails - fits) / 2;
                    if (packPage(b, v, first, mid, &page)) fits = mid; else fails = mid;
                }
                end = fits;
                packPage(b, v, first, end, &page);
            }
        }
        b->pages[b->pageCount++] = page;
        first = end;
    }
}

// The real packer, on an ASS_Image list whose bitmap pointers stand for the dump's uids (packing
// never reads the pixels).
static void packFrameWithAssAtlas(Bench* b, AssAtlas* atlas, ASS_Image* images, const unsigned char* uidBitmaps) {
    for (int i = 0; i < b->count; i++) {
        const DumpPiece* p = &b->pieces[i];
        images[i] = (ASS_Image) {.w = p->w, .h = p->h, .stride = p->w, .bitmap = (unsigned char*) &uidBitmaps[p->uid],
            .dst_x = p->dstX, .dst_y = p->dstY, .next = i + 1 < b->count ? &images[i + 1] : NULL};
    }
    assAtlasPack(atlas, images);
    if (atlas->pieceCount != b->count) {
        fprintf(stderr, "assAtlasPack kept %d of %d pieces\n", atlas->pieceCount, b->count);
        exit(1);
    }
    for (int i = 0; i < b->count; i++) {
        b->x[i] = atlas->pieces[i].atlasX;
        b->y[i] = atlas->pieces[i].atlasY;
    }
    b->pageCount = atlas->pageCount;
    for (int i = 0; i < atlas->pageCount; i++) {
        const AssAtlasPage* pg = &atlas->pages[i];
        b->pages[i] = (Page) {pg->firstPiece, pg->pieceCount, pg->usedW, pg->usedH};
    }
}

static void checkLayout(const Bench* b, const char* variant, const char* dump, int frameIdx) {
    int next = 0;
    for (int pageIdx = 0; pageIdx < b->pageCount; pageIdx++) {
        const Page* pg = &b->pages[pageIdx];
        bool ok = pg->first == next && pg->count > 0 && pg->usedW <= b->maxSize && pg->usedH <= b->maxSize;
        for (int i = pg->first; ok && i < pg->first + pg->count; i++) {
            const DumpPiece* p = &b->pieces[i];
            ok = b->x[i] >= 0 && b->y[i] >= 0 && b->x[i] + p->w <= pg->usedW && b->y[i] + p->h <= pg->usedH;
            for (int j = pg->first; ok && j < i; j++) {
                const DumpPiece* q = &b->pieces[j];
                const bool overlap = b->x[i] < b->x[j] + q->w && b->x[j] < b->x[i] + p->w && b->y[i] < b->y[j] + q->h && b->y[j] < b->y[i] + p->h;
                ok = !overlap || (p->uid == q->uid && b->x[i] == b->x[j] && b->y[i] == b->y[j]);
            }
        }
        if (!ok) {
            fprintf(stderr, "%s: invalid layout for %s frame %d, page %d\n", variant, dump, frameIdx, pageIdx);
            exit(1);
        }
        next += pg->count;
    }
    if (next != b->count) {
        fprintf(stderr, "%s: pages don't cover %s frame %d\n", variant, dump, frameIdx);
        exit(1);
    }
}

int main(int argc, char** argv) {
    if (argc < 3 || atoi(argv[1]) <= 0) {
        fprintf(stderr, "usage: %s <max-atlas-size> <dump>...\n", argv[0]);
        return 2;
    }
    const int maxSize = atoi(argv[1]);
    for (int dumpIdx = 2; dumpIdx < argc; dumpIdx++) {
        const char* name = frameDumpName(argv[dumpIdx]);
        FrameDump dump = loadFrameDump(argv[dumpIdx]);
        // Pieces larger than a page can't be placed: like assAtlasPack, every packer goes without them.
        int maxCount = 0, maxUnique = 0, frames = 0, dropped = 0;
        long long* distinctTexels = allocOrDie((size_t) dump.frameCount * sizeof(long long)); // per frame
        for (int f = 0; f < dump.frameCount; f++) {
            DumpFrame* frame = &dump.frames[f];
            bool* seen = calloc((size_t) frame->uniqueCount + 1, sizeof(bool));
            if (seen == NULL) return 2;
            int kept = 0;
            distinctTexels[f] = 0;
            for (int i = 0; i < frame->count; i++) {
                const DumpPiece piece = frame->pieces[i];
                if (piece.w > maxSize || piece.h > maxSize) {
                    dropped++;
                    continue;
                }
                frame->pieces[kept++] = piece;
                if (!seen[piece.uid]) distinctTexels[f] += (long long) piece.w * piece.h;
                seen[piece.uid] = true;
            }
            free(seen);
            frame->count = kept;
            if (frame->count > maxCount) maxCount = frame->count;
            if (frame->uniqueCount > maxUnique) maxUnique = frame->uniqueCount;
            if (frame->count > 0) frames++;
        }
        printf("== %s (%dx%d, %d frames with subtitles), pages up to %d", name, dump.width, dump.height, frames, maxSize);
        if (dropped > 0) printf(", %d pieces larger than a page dropped", dropped);
        printf("\n");
        if (frames == 0) continue;
        printf("%-31s %14s %9s %7s %6s %10s %9s %9s\n", "packer", "upload/frame", "/distinct", "worst", "pages", "multi-page", "pack avg", "max");

        Bench b = {.maxSize = maxSize};
        b.sameAs = allocOrDie((size_t) maxCount * sizeof(int));
        b.lastOfUid = allocOrDie((size_t) maxUnique * sizeof(int));
        b.x = allocOrDie((size_t) maxCount * sizeof(int));
        b.y = allocOrDie((size_t) maxCount * sizeof(int));
        b.tryX = allocOrDie((size_t) maxCount * sizeof(int));
        b.tryY = allocOrDie((size_t) maxCount * sizeof(int));
        b.run = allocOrDie((size_t) maxCount * sizeof(int));
        b.order = allocOrDie((size_t) maxCount * sizeof(OrderEntry));
        b.nodes = allocOrDie((size_t) (2 * maxCount + 2) * sizeof(Node));
        b.shelves = allocOrDie((size_t) maxCount * sizeof(Rect));
        b.pages = allocOrDie((size_t) maxCount * sizeof(Page));
        ASS_Image* images = allocOrDie((size_t) maxCount * sizeof(ASS_Image));
        unsigned char* uidBitmaps = allocOrDie((size_t) maxUnique); // one stand-in pixels pointer per uid

        for (size_t v = 0; v < sizeof(kVariants) / sizeof(kVariants[0]); v++) {
            const Variant* variant = &kVariants[v];
            AssAtlas atlas = {.maxPageSize = maxSize};
            long long uploadTexels = 0, distinctTotal = 0, totalNs = 0, maxNs = 0;
            double worst = 0;
            int maxPages = 0, multiPageFrames = 0;
            for (int f = 0; f < dump.frameCount; f++) {
                const DumpFrame* frame = &dump.frames[f];
                if (frame->count == 0) continue;
                b.pieces = frame->pieces;
                b.count = frame->count;
                const long long start = nowNs();
                if (variant->assAtlasPack) packFrameWithAssAtlas(&b, &atlas, images, uidBitmaps);
                else packFrame(&b, variant, frame);
                const long long elapsed = nowNs() - start;
                totalNs += elapsed;
                if (elapsed > maxNs) maxNs = elapsed;
                checkLayout(&b, variant->name, name, f);

                long long frameUpload = 0;
                for (int p = 0; p < b.pageCount; p++) frameUpload += (long long) b.pages[p].usedW * b.pages[p].usedH;
                uploadTexels += frameUpload;
                distinctTotal += distinctTexels[f];
                if ((double) frameUpload / distinctTexels[f] > worst) worst = (double) frameUpload / distinctTexels[f];
                if (b.pageCount > maxPages) maxPages = b.pageCount;
                if (b.pageCount > 1) multiPageFrames++;
            }
            assAtlasFree(&atlas);
            printf("%-31s %11.2f MB %8.1f%% %6.0f%% %6d %10d %6.0f us %6.0f us\n", variant->name, uploadTexels / 1e6 / frames,
                100.0 * uploadTexels / distinctTotal, 100 * worst, maxPages, multiPageFrames, totalNs / 1e3 / frames, maxNs / 1e3);
        }
        free(b.sameAs);
        free(b.lastOfUid);
        free(b.x);
        free(b.y);
        free(b.tryX);
        free(b.tryY);
        free(b.run);
        free(b.order);
        free(b.nodes);
        free(b.shelves);
        free(b.pages);
        free(b.freeRects);
        free(images);
        free(uidBitmaps);
        free(distinctTexels);
        freeFrameDump(&dump);
    }
    return 0;
}
