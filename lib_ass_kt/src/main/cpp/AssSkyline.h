#ifndef LIBASS_ANDROID_ASS_SKYLINE_H
#define LIBASS_ANDROID_ASS_SKYLINE_H

#include <stdbool.h>

// One step of the skyline: the packed height over the columns [x, next node's x).
typedef struct {
    int x, y;
} AssSkylineNode;

// Packs rectangles into a width x maxHeight area, each at the lowest spot it fits, then the
// leftmost. Only the top edge of what is packed is tracked - the skyline - so the space under a
// rectangle that overhangs a lower one is lost; fed tallest first, little is.
typedef struct {
    AssSkylineNode* nodes; // nodes[count] marks the right edge
    int count;
    int width, maxHeight;
} AssSkyline;

// Starts an empty skyline in `storage`, which must hold two nodes more than the rectangles to pack.
void assSkylineReset(AssSkyline* skyline, AssSkylineNode* storage, int width, int maxHeight);

// Places a w x h rectangle and returns its position, or false if it doesn't fit.
bool assSkylineAdd(AssSkyline* skyline, int w, int h, int* x, int* y);

#endif // LIBASS_ANDROID_ASS_SKYLINE_H
