#ifndef LIBASS_ANDROID_ASS_ATLAS_H
#define LIBASS_ANDROID_ASS_ATLAS_H

#include <stdbool.h>
#include <stdint.h>

#include "AssSkyline.h"
#include "ass/ass.h"

// Where a frame's subtitle bitmaps go in the atlas pages. CPU side only: AssOverlayGl.c uploads
// each page into a texture.

// One libass image, and where its bitmap sits in its page.
typedef struct {
    int atlasX, atlasY;
    int dstX, dstY, w, h;
    uint32_t color; // libass RGBA, A being transparency
    // Only read during the ass_render_frame call that produced it; kept after that to recognize the
    // same bitmap in later frames.
    const unsigned char* bitmap;
    int stride;
    // The closest earlier piece showing the very same bitmap, or -1. When that one is on the same
    // page, this piece shares its texels instead of being stored again.
    int sameAs;
} AssAtlasPiece;

// A page holds a contiguous run of the pieces. Pages are drawn in order, one draw call each, so the
// pieces are painted in libass's list order however many pages they take.
typedef struct {
    int firstPiece, pieceCount;
    int usedW, usedH; // the packed area, from (0, 0)
} AssAtlasPage;

typedef struct AssAtlasPackEntry AssAtlasPackEntry;

typedef struct {
    int maxPageSize; // the largest texture the GPU takes
    AssAtlasPiece* pieces; // in libass list order, which is the paint order
    int pieceCount, pieceCap;
    AssAtlasPage* pages;
    int pageCount, pageCap;

    // Packing scratch, sized along with the pieces.
    AssAtlasPackEntry* packOrder;
    AssSkylineNode* skylineNodes;
    int* bitmapSlots; // hash table of piece indices, -1 when empty
    int bitmapSlotCap; // a power of two, at least twice pieceCap
} AssAtlas;

// Lays out `images` from scratch. Images larger than a page are dropped, which takes a frame larger
// than the GPU's max texture size. On OOM, fewer images are laid out, possibly none.
void assAtlasPack(AssAtlas* atlas, const ASS_Image* images);

// Moves the pieces to the destinations of `images`, keeping the layout: for frames where libass
// reports that only positions changed. Returns false if `images` doesn't show the same bitmaps.
bool assAtlasMovePieces(AssAtlas* atlas, const ASS_Image* images);

// Writes a page's bitmaps into `buf`, a usedW x usedH area with rows usedW bytes apart. Bytes
// between the pieces are left as they are: they are never sampled.
void assAtlasFillPage(const AssAtlas* atlas, const AssAtlasPage* page, unsigned char* buf);

// The bitmap texels a page stores.
long long assAtlasPageTexels(const AssAtlas* atlas, const AssAtlasPage* page);

// Drops the pages from `pageCount` on, with their pieces.
void assAtlasTruncate(AssAtlas* atlas, int pageCount);

void assAtlasFree(AssAtlas* atlas);

#endif // LIBASS_ANDROID_ASS_ATLAS_H
