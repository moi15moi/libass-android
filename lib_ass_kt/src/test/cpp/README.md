# Overlay host tools

Off-device tests and benchmarks for the `EFFECTS_ATLAS` overlay
([`AssOverlay.c`](../../main/cpp/AssOverlay.c) and the files it uses), on real libass output. They
compile the overlay's real sources for the host, with stand-ins for `android/log.h` and `jni.h`
([`stubs/`](stubs)).

## Requirements

Linux with a C compiler, make, pkg-config, libass, Mesa (EGL and GLES 3 headers and libraries), and
MKVToolNix. `test_gl` runs on Mesa's surfaceless EGL platform with its llvmpipe software renderer,
so it needs no GPU.

## Usage

```sh
cd lib_ass_kt/src/test/cpp
./run.sh prepare             # extract the subtitles and fonts of the app's test videos, record frame dumps
./run.sh check               # lay out every recorded frame with the atlas and check the result
./run.sh check-gl            # draw every track through the real GL path and check every frame
./run.sh compare <git-rev>   # check-gl, and check if it can, side by side with <git-rev>'s overlay
./run.sh bench               # compare packing strategies
./run.sh stats               # describe the recorded frames
```

`run.sh` builds the tools it needs (`make` builds them all). `prepare` also takes other MKV files as
arguments. Everything is written to `build/`, which git ignores. On 4 cores, `prepare`, `check` and
`bench` take about a minute each, `check-gl` about 5 minutes, and `compare` about 12.

Environment variables:

- `RESOLUTIONS`: frame sizes `prepare` records dumps at (default `1920x1080 3840x2160`).
- `MAX_SIZES`: page size limits `check` and `bench` pack at (default `16384 8192 4096 2048`). The
  device's limit is `GL_MAX_TEXTURE_SIZE`; the lower ones force multi-page frames.
- `JOBS`: how many tests run in parallel (default: the CPU count). Use `JOBS=1` for steady timings.

## Tools

| Tool | What it does |
|---|---|
| `dump_frames` | Renders a track with libass the way the app does, and records every frame that makes the overlay repack its atlas: the images' sizes, positions and bitmap identities ([`frame_dump.h`](frame_dump.h)). |
| `test_pack` | Lays out every recorded frame with `assAtlasPack`, fills the pages with `assAtlasFillPage`, and checks every piece's texels, the page bounds, overlaps and list order, and `assAtlasMovePieces`. Reports the upload size and packing time. |
| `test_gl` | Draws a track with `nativeAssOverlayDraw` on a headless GLES 3 context and compares every frame with a CPU compositing of the same images. Reports the texels uploaded per repack, the pages drawn, and a hash of all frames. `-m` lowers the reported max texture size to force multi-page frames, `-2` reports GLES 2 for its upload path. |
| `bench_packers` | Compares `assAtlasPack` with other page packers, piece orders and ways of splitting pages. |
| `frame_stats` | Pieces and texels per repack, how much repeats a bitmap of the same frame, and how much the previous repack already had. |

## Notes

- `test_pack` and `bench_packers` only build the atlas layout ([`AssAtlas.c`](../../main/cpp/AssAtlas.c)),
  with no GL. `test_pack` fills the bitmaps with pseudo-random pixels, and several images share one
  bitmap wherever libass made them share one.
- `test_gl` only uses the JNI entry points: it steers and watches the overlay through a few GL calls
  wrapped at link time. It builds against any version of the overlay, so `compare` always runs it;
  `test_pack` needs the atlas API, so `compare` skips it for older revisions.
- The rendered frames don't depend on how the atlas is packed: for a given track, the three
  `check-gl` configurations print the same hash, and a packing change should leave `compare`'s
  `test_gl` runs at "same pixels" while the upload numbers move.
