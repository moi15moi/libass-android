// Describes frame dumps (see frame_dump.h): how many pieces and texels each repack carries, how much
// of that repeats a bitmap already in the frame (AssOverlay.c stores those once per page), and how
// much of a frame's distinct bitmap texels the previous repack already had - what an atlas kept
// across frames could skip uploading.
//
// usage: frame_stats <dump>...
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame_dump.h"

static int compareInts(const void* a, const void* b) {
    const int x = *(const int*) a, y = *(const int*) b;
    return (x > y) - (x < y);
}

static int compareLongs(const void* a, const void* b) {
    const long long x = *(const long long*) a, y = *(const long long*) b;
    return (x > y) - (x < y);
}

static int compareHashes(const void* a, const void* b) {
    const uint64_t x = *(const uint64_t*) a, y = *(const uint64_t*) b;
    return (x > y) - (x < y);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <dump>...\n", argv[0]);
        return 2;
    }
    printf("%-34s %9s %6s  %-17s  %-10s  %-20s  %-11s  %7s  %8s\n", "dump", "size", "frames",
        "pieces p50/95/max", "distinct", "Mtexels p50/95/max", "widest/tall", "repeats", "in prev.");
    for (int dumpIdx = 1; dumpIdx < argc; dumpIdx++) {
        FrameDump dump = loadFrameDump(argv[dumpIdx]);
        int* counts = (int*) malloc((size_t) (dump.frameCount + 1) * sizeof(int));
        long long* texels = (long long*) malloc((size_t) (dump.frameCount + 1) * sizeof(long long));
        uint64_t* previousHashes = NULL; // the previous frame's distinct bitmaps, sorted
        int previousCount = 0;
        int frames = 0, distinctSum = 0, distinctMax = 0, widest = 0, tallest = 0;
        long long totalTexels = 0, repeatTexels = 0, reusedTexels = 0, reuseBase = 0;
        for (int frameIdx = 0; frameIdx < dump.frameCount; frameIdx++) {
            const DumpFrame* frame = &dump.frames[frameIdx];
            if (frame->count == 0) continue;
            bool* seen = (bool*) calloc((size_t) frame->uniqueCount, sizeof(bool));
            uint64_t* hashes = (uint64_t*) malloc((size_t) frame->uniqueCount * sizeof(uint64_t));
            if (counts == NULL || texels == NULL || seen == NULL || hashes == NULL) {
                fprintf(stderr, "out of memory\n");
                return 2;
            }
            long long frameTexels = 0, frameDistinct = 0, frameReused = 0;
            for (int i = 0; i < frame->count; i++) {
                const DumpPiece* piece = &frame->pieces[i];
                const long long area = (long long) piece->w * piece->h;
                frameTexels += area;
                if (piece->w > widest) widest = piece->w;
                if (piece->h > tallest) tallest = piece->h;
                if (seen[piece->uid]) {
                    repeatTexels += area;
                    continue;
                }
                seen[piece->uid] = true;
                hashes[piece->uid] = piece->hash;
                frameDistinct += area;
                if (previousCount > 0 && bsearch(&piece->hash, previousHashes, (size_t) previousCount, sizeof(uint64_t), compareHashes)) {
                    frameReused += area;
                }
            }
            if (previousCount > 0) {
                reusedTexels += frameReused;
                reuseBase += frameDistinct;
            }
            qsort(hashes, (size_t) frame->uniqueCount, sizeof(uint64_t), compareHashes);
            free(previousHashes);
            previousHashes = hashes;
            previousCount = frame->uniqueCount;
            free(seen);

            counts[frames] = frame->count;
            texels[frames] = frameTexels;
            frames++;
            totalTexels += frameTexels;
            distinctSum += frame->uniqueCount;
            if (frame->uniqueCount > distinctMax) distinctMax = frame->uniqueCount;
        }

        char size[32];
        snprintf(size, sizeof(size), "%dx%d", dump.width, dump.height);
        if (frames == 0) {
            printf("%-34s %9s %6d  no frames with subtitles\n", frameDumpName(argv[dumpIdx]), size, 0);
        } else {
            qsort(counts, (size_t) frames, sizeof(int), compareInts);
            qsort(texels, (size_t) frames, sizeof(long long), compareLongs);
            char pieces[32], distinct[32], area[48], extent[32];
            snprintf(pieces, sizeof(pieces), "%d/%d/%d", counts[frames / 2], counts[frames * 95 / 100], counts[frames - 1]);
            snprintf(distinct, sizeof(distinct), "%.0f/%d", (double) distinctSum / frames, distinctMax);
            snprintf(area, sizeof(area), "%.1f/%.1f/%.1f", texels[frames / 2] / 1e6, texels[frames * 95 / 100] / 1e6, texels[frames - 1] / 1e6);
            snprintf(extent, sizeof(extent), "%d/%d", widest, tallest);
            printf("%-34s %9s %6d  %-17s  %-10s  %-20s  %-11s  %6.1f%%  %7.1f%%\n", frameDumpName(argv[dumpIdx]), size,
                frames, pieces, distinct, area, extent, 100.0 * repeatTexels / totalTexels,
                reuseBase > 0 ? 100.0 * reusedTexels / reuseBase : 0.0);
        }
        free(previousHashes);
        free(counts);
        free(texels);
        freeFrameDump(&dump);
    }
    printf("\npieces: images per repack; distinct: different bitmaps among them (avg/max); repeats: share of\n"
           "texels repeating a bitmap of the same frame; in prev.: share of the distinct texels the previous\n"
           "repack already had.\n");
    return 0;
}
