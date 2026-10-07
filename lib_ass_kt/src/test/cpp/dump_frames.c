// Renders a subtitle track with libass the way the app does, and records every frame that makes
// AssOverlay.c repack its atlas (format: frame_dump.h).
//
// usage: dump_frames <track.ass> <fonts-dir> <width>x<height> <out.dump> [fps]
//
// Frames are rendered at `fps` (default 23.976) from 0 to the end of the track's last event. A frame
// is recorded when libass reports new content (changed == 2), or a position-only change
// (changed == 1) that the overlay can't apply in place because the bitmaps differ from the previous
// frame's - the same rule as assOverlayUpdate.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame_dump.h"
#include "libass_track.h"

typedef struct {
    const unsigned char* bitmap;
    int w, h, stride;
} BitmapKey;

// FNV-1a over the size and the visible pixels (not the stride padding).
static uint64_t hashBitmap(const ASS_Image* img) {
    uint64_t hash = 14695981039346656037ULL;
    hash = (hash ^ (uint64_t) img->w) * 1099511628211ULL;
    hash = (hash ^ (uint64_t) img->h) * 1099511628211ULL;
    for (int y = 0; y < img->h; y++) {
        const unsigned char* row = img->bitmap + (size_t) y * img->stride;
        for (int x = 0; x < img->w; x++) hash = (hash ^ row[x]) * 1099511628211ULL;
    }
    return hash;
}

static void* growArray(void* array, int* cap, int needed, size_t elemSize) {
    if (needed <= *cap) return array;
    int newCap = *cap > 0 ? *cap : 64;
    while (newCap < needed) newCap *= 2;
    array = realloc(array, (size_t) newCap * elemSize);
    if (array == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    *cap = newCap;
    return array;
}

static void writeInts(FILE* out, const int32_t* values, size_t count) {
    if (fwrite(values, sizeof(int32_t), count, out) != count) {
        perror("write");
        exit(2);
    }
}

int main(int argc, char** argv) {
    int width, height;
    if (argc < 5 || argc > 6 || sscanf(argv[3], "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0) {
        fprintf(stderr, "usage: %s <track.ass> <fonts-dir> <width>x<height> <out.dump> [fps]\n", argv[0]);
        return 2;
    }
    const double fps = argc > 5 ? atof(argv[5]) : 23.976;
    LibassTrack t = openLibassTrack(argv[1], argv[2]);
    // What nativeAssOverlayDraw sets up for a video frame of this size.
    ass_set_storage_size(t.renderer, width, height);
    ass_set_frame_size(t.renderer, width, height);

    FILE* out = fopen(argv[4], "wb");
    if (out == NULL) {
        perror(argv[4]);
        return 2;
    }
    fwrite(FRAME_DUMP_MAGIC, 1, 4, out);
    writeInts(out, (const int32_t[]) {FRAME_DUMP_VERSION, width, height}, 3);

    BitmapKey* keys = NULL; // the frame's pieces
    int* uids = NULL;
    int keyCap = 0, uidCap = 0;
    uint64_t* hashes = NULL; // per uid
    int hashCap = 0;
    BitmapKey* previous = NULL; // the last recorded frame's pieces, for the changed == 1 rule
    int previousCap = 0, previousCount = -1;
    long long frames = 0, recorded = 0, positionOnly = 0;
    for (long long frameIdx = 0;; frameIdx++) {
        const long long timeMs = frameTimeMs(frameIdx, fps);
        if (timeMs > t.endMs) break;
        frames++;
        int changed = 0;
        ASS_Image* image = ass_render_frame(t.renderer, t.track, timeMs, &changed);
        if (changed == 0) continue;

        int count = 0, uniqueCount = 0;
        for (ASS_Image* img = image; img != NULL; img = img->next) {
            if (img->w <= 0 || img->h <= 0) continue;
            keys = growArray(keys, &keyCap, count + 1, sizeof(BitmapKey));
            uids = growArray(uids, &uidCap, count + 1, sizeof(int));
            keys[count] = (BitmapKey) {img->bitmap, img->w, img->h, img->stride};
            uids[count] = -1;
            for (int other = 0; other < count && uids[count] < 0; other++) {
                const BitmapKey* a = &keys[other];
                if (a->bitmap == img->bitmap && a->w == img->w && a->h == img->h && a->stride == img->stride) uids[count] = uids[other];
            }
            if (uids[count] < 0) {
                hashes = growArray(hashes, &hashCap, uniqueCount + 1, sizeof(uint64_t));
                hashes[uniqueCount] = hashBitmap(img);
                uids[count] = uniqueCount++;
            }
            count++;
        }

        if (changed == 1 && count == previousCount) {
            bool same = true;
            for (int i = 0; i < count && same; i++) {
                same = keys[i].bitmap == previous[i].bitmap && keys[i].w == previous[i].w && keys[i].h == previous[i].h;
            }
            if (same) {
                positionOnly++;
                continue;
            }
        }
        previous = growArray(previous, &previousCap, count, sizeof(BitmapKey));
        memcpy(previous, keys, (size_t) count * sizeof(BitmapKey));
        previousCount = count;

        writeInts(out, (const int32_t[]) {(int32_t) timeMs, changed, count}, 3);
        int pieceIdx = 0;
        for (ASS_Image* img = image; img != NULL; img = img->next) {
            if (img->w <= 0 || img->h <= 0) continue;
            const int uid = uids[pieceIdx++];
            writeInts(out, (const int32_t[]) {img->w, img->h, img->dst_x, img->dst_y, uid,
                (int32_t) (uint32_t) hashes[uid], (int32_t) (uint32_t) (hashes[uid] >> 32)}, 7);
        }
        recorded++;
    }
    if (fclose(out) != 0) {
        perror(argv[4]);
        return 2;
    }
    fprintf(stderr, "%s @%dx%d: %lld frames, %lld recorded, %lld position-only\n",
        argv[1], width, height, frames, recorded, positionOnly);

    free(keys);
    free(uids);
    free(hashes);
    free(previous);
    closeLibassTrack(&t);
    return 0;
}
