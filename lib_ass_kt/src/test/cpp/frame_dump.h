// Frame dumps: what libass hands AssOverlay.c on every frame that makes it repack its atlas,
// recorded by dump_frames from a real subtitle track so the packer can be tested and benchmarked
// off-device.
//
// File layout, little-endian:
//   char magic[4] = "ASSD"; int32 version = 1; int32 frameWidth, frameHeight;
//   then for every recorded frame: int32 timeMs, changed, pieceCount; followed by pieceCount
//   pieces of int32 w, h, dstX, dstY, uid; uint32 hashLo, hashHi.
//
// Pieces are the frame's non-empty ASS_Images, in list order. `uid` numbers the frame's distinct
// bitmaps (same pixels pointer, size and stride) from 0 in order of first appearance, so equal uids
// are repeats of one bitmap. `hash` is a hash of the bitmap's size and pixels, comparable across
// frames.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_DUMP_MAGIC "ASSD"
#define FRAME_DUMP_VERSION 1

typedef struct {
    int w, h, dstX, dstY, uid;
    uint64_t hash;
} DumpPiece;

typedef struct {
    int timeMs, changed, count;
    int uniqueCount; // distinct bitmaps, i.e. highest uid + 1
    DumpPiece* pieces;
} DumpFrame;

typedef struct {
    int width, height;
    int frameCount;
    DumpFrame* frames;
} FrameDump;

static inline void frameDumpFail(const char* path, const char* what) {
    fprintf(stderr, "%s: %s\n", path, what);
    exit(2);
}

// Loads a whole dump. These are test tools: any error ends the process.
static inline FrameDump loadFrameDump(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) frameDumpFail(path, "cannot open");
    char magic[4];
    int32_t header[3];
    if (fread(magic, 1, 4, file) != 4 || memcmp(magic, FRAME_DUMP_MAGIC, 4) != 0) frameDumpFail(path, "not a frame dump");
    if (fread(header, sizeof(header), 1, file) != 1 || header[0] != FRAME_DUMP_VERSION) {
        frameDumpFail(path, "unsupported frame dump version: run dump_frames again");
    }
    FrameDump dump = {header[1], header[2], 0, NULL};
    int cap = 0;
    int32_t frameHeader[3];
    while (fread(frameHeader, sizeof(frameHeader), 1, file) == 1) {
        if (dump.frameCount == cap) {
            cap = cap > 0 ? cap * 2 : 256;
            dump.frames = (DumpFrame*) realloc(dump.frames, (size_t) cap * sizeof(DumpFrame));
            if (dump.frames == NULL) frameDumpFail(path, "out of memory");
        }
        DumpFrame* frame = &dump.frames[dump.frameCount++];
        frame->timeMs = frameHeader[0];
        frame->changed = frameHeader[1];
        frame->count = frameHeader[2];
        frame->uniqueCount = 0;
        frame->pieces = (DumpPiece*) malloc((size_t) (frame->count > 0 ? frame->count : 1) * sizeof(DumpPiece));
        if (frame->pieces == NULL) frameDumpFail(path, "out of memory");
        for (int i = 0; i < frame->count; i++) {
            int32_t record[7];
            if (fread(record, sizeof(record), 1, file) != 1) frameDumpFail(path, "truncated");
            DumpPiece* piece = &frame->pieces[i];
            piece->w = record[0];
            piece->h = record[1];
            piece->dstX = record[2];
            piece->dstY = record[3];
            piece->uid = record[4];
            piece->hash = (uint64_t) (uint32_t) record[5] | (uint64_t) (uint32_t) record[6] << 32;
            if (piece->uid < 0 || piece->uid > frame->uniqueCount) frameDumpFail(path, "corrupt piece uid");
            if (piece->uid + 1 > frame->uniqueCount) frame->uniqueCount = piece->uid + 1;
        }
    }
    fclose(file);
    return dump;
}

static inline void freeFrameDump(FrameDump* dump) {
    for (int i = 0; i < dump->frameCount; i++) free(dump->frames[i].pieces);
    free(dump->frames);
    dump->frames = NULL;
    dump->frameCount = 0;
}

// The file name without its directory, for reports.
static inline const char* frameDumpName(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}
