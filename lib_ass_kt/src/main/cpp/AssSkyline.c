#include "AssSkyline.h"

#include <stddef.h>
#include <string.h>

void assSkylineReset(AssSkyline* skyline, AssSkylineNode* storage, int width, int maxHeight) {
    skyline->nodes = storage;
    skyline->nodes[0] = (AssSkylineNode) {0, 0};
    skyline->nodes[1] = (AssSkylineNode) {width, 0};
    skyline->count = 1;
    skyline->width = width;
    skyline->maxHeight = maxHeight;
}

// Finds the node a w x h rectangle rests lowest on - then leftmost - with its left edge on the
// node's. Returns that node and the resting height, or -1 if the rectangle fits nowhere.
static int findLowestNode(const AssSkyline* skyline, int w, int h, int* restY) {
    const AssSkylineNode* nodes = skyline->nodes;
    int bestNode = -1;
    int bestY = skyline->maxHeight - h + 1; // resting this high or higher would overflow
    for (int i = 0; i < skyline->count && nodes[i].x + w <= skyline->width; i++) {
        // The rectangle rests on the highest node under it. Later candidates are further right, so
        // only a strictly lower one can win: stop as soon as this one can't.
        const int right = nodes[i].x + w;
        int y = 0;
        for (int k = i; nodes[k].x < right && y < bestY; k++) {
            if (nodes[k].y > y) y = nodes[k].y;
        }
        if (y < bestY) {
            bestY = y;
            bestNode = i;
        }
    }
    *restY = bestY;
    return bestNode;
}

static void removeNode(AssSkyline* skyline, int i) {
    memmove(&skyline->nodes[i], &skyline->nodes[i + 1], (size_t) (skyline->count - i) * sizeof(AssSkylineNode));
    skyline->count--;
}

// Raises the columns under a w-wide rectangle on node i to `top`. The last node it only partly
// covers keeps its uncovered part, and level neighbors merge, so neighbors always differ in height.
static void raiseColumns(AssSkyline* skyline, int i, int w, int top) {
    AssSkylineNode* nodes = skyline->nodes;
    const int right = nodes[i].x + w;
    int end = i + 1; // the first node from the rectangle's right edge on
    while (nodes[end].x < right) end++;
    const bool partial = nodes[end].x > right;
    const int restY = nodes[end - 1].y;

    // Nodes [i, end) become node i, plus the uncovered part.
    const int kept = partial ? 2 : 1;
    memmove(&nodes[i + kept], &nodes[end], (size_t) (skyline->count + 1 - end) * sizeof(AssSkylineNode));
    skyline->count += kept - (end - i);
    nodes[i].y = top;
    if (partial) {
        nodes[i + 1] = (AssSkylineNode) {right, restY}; // below `top`, so it can't merge
    } else if (i + 1 < skyline->count && nodes[i + 1].y == top) {
        removeNode(skyline, i + 1);
    }
    if (i > 0 && nodes[i - 1].y == top) removeNode(skyline, i);
}

bool assSkylineAdd(AssSkyline* skyline, int w, int h, int* x, int* y) {
    int restY;
    const int node = findLowestNode(skyline, w, h, &restY);
    if (node < 0) return false;
    *x = skyline->nodes[node].x;
    *y = restY;
    raiseColumns(skyline, node, w, restY + h);
    return true;
}
