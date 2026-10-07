#!/usr/bin/env bash
# Workflows for the overlay's host tools (see README.md):
#
#   run.sh prepare [file.mkv...]  extract the subtitle tracks and fonts of the MKVs (default: the
#                                 app's test videos) and record their frame dumps
#   run.sh check                  test_pack on every dump, at each page size limit in MAX_SIZES
#   run.sh check-gl               test_gl on every track at 1080p: default pages, pages capped to
#                                 2048, and the GLES2 upload path
#   run.sh bench                  bench_packers on every dump, at each page size limit in MAX_SIZES
#   run.sh stats                  frame_stats on every dump
#   run.sh compare <git-rev>      check-gl (and check, if it has the atlas API) side by side with
#                                 <git-rev>'s overlay
set -euo pipefail
cd "$(dirname "$0")"

BIN=build/bin
DATA=build/data
RESOLUTIONS=${RESOLUTIONS:-1920x1080 3840x2160}
MAX_SIZES=${MAX_SIZES:-16384 8192 4096 2048}
JOBS=${JOBS:-$(nproc)}
# test_gl renders with Mesa's software renderer: no GPU needed, and deterministic output.
export LIBGL_ALWAYS_SOFTWARE=1
export EGL_LOG_LEVEL=${EGL_LOG_LEVEL:-fatal}
if [ -f /usr/share/glvnd/egl_vendor.d/50_mesa.json ]; then
    export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
fi

die() {
    echo "$*" >&2
    exit 1
}

findDumps() {
    DUMPS=("$DATA"/dumps/*.dump)
    [ -e "${DUMPS[0]}" ] || die "no frame dumps: run '$0 prepare' first"
}

findTracks() {
    TRACKS=("$DATA"/*/track*.ass)
    [ -e "${TRACKS[0]}" ] || die "no tracks: run '$0 prepare' first"
}

prepare() {
    local mkvs=("$@")
    [ ${#mkvs[@]} -gt 0 ] || mkvs=(../../../../app/src/main/assets/*.mkv)
    make -s "$BIN/dump_frames"
    mkdir -p "$DATA/dumps"
    local pids=()
    for mkv in "${mkvs[@]}"; do
        local name dir mkvPath
        name=$(basename "$mkv" .mkv)
        name=${name// /_}
        dir=$DATA/$name
        mkvPath=$(realpath "$mkv")
        rm -rf "$dir"
        mkdir -p "$dir/fonts"
        for id in $(mkvmerge --ui-language en_US -i "$mkv" | sed -n 's/^Track ID \([0-9]*\): subtitles (SubStationAlpha)$/\1/p'); do
            mkvextract "$mkv" tracks "$id:$dir/track$id.ass" > /dev/null
        done
        # Attachments (fonts, in practice) are numbered from 1.
        (cd "$dir/fonts" && for ((id = 1; ; id++)); do mkvextract "$mkvPath" attachments "$id" > /dev/null 2>&1 || break; done)
        for track in "$dir"/track*.ass; do
            [ -e "$track" ] || continue
            for resolution in $RESOLUTIONS; do
                "$BIN/dump_frames" "$track" "$dir/fonts" "$resolution" \
                    "$DATA/dumps/${name}_$(basename "$track" .ass)_$resolution.dump" &
                pids+=($!)
            done
        done
    done
    local failed=0
    for pid in "${pids[@]}"; do wait "$pid" || failed=1; done
    [ $failed -eq 0 ] || die "dump_frames failed"
}

# inParallel <command> <arguments>...: runs "<command> <arguments>" once per argument string (both
# split into words), JOBS at a time. Prints the outputs in argument order; fails if any run did.
inParallel() {
    local command=$1 pids=() out failed=0
    shift
    out=$(mktemp -d)
    for arguments in "$@"; do
        while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 0.1; done
        # shellcheck disable=SC2086 # both hold several words
        $command $arguments > "$out/${#pids[@]}" &
        pids+=($!)
    done
    for i in "${!pids[@]}"; do
        wait "${pids[$i]}" || failed=1
        if [ -s "$out/$i" ]; then cat "$out/$i"; else echo "$command ${*:i+1:1}: no result"; fi
    done
    rm -rf "$out"
    return $failed
}

# packChecks <bin-dir>: test_pack on every dump, at every MAX_SIZES limit.
packChecks() {
    inParallel "$1/test_pack ${MAX_SIZES// /,}" "${DUMPS[@]}"
}

# glChecks <bin-dir>: test_gl on every track at 1080p, in each configuration.
glChecks() {
    local runs=()
    for track in "${TRACKS[@]}"; do
        for options in "" "-m 2048" "-m 2048 -2"; do runs+=("$options $track $(dirname "$track")/fonts 1920x1080"); done
    done
    inParallel "$1/test_gl" "${runs[@]}"
}

compare() {
    local rev=${1:-}
    [ -n "$rev" ] || die "usage: $0 compare <git-rev>"
    local sources=lib_ass_kt/src/main/cpp root base overlayDir
    root=$(git rev-parse --show-toplevel)
    base=build/base-$(git rev-parse --short "$rev")
    rm -rf "$base"
    mkdir -p "$base/src"
    for file in $(git -C "$root" ls-tree --name-only "$rev" "$sources/"); do
        git -C "$root" show "$rev:$file" > "$base/src/$(basename "$file")"
    done
    overlayDir=$(realpath "$base/src")
    make -s "$BIN/test_gl"
    make -s OVERLAY_DIR="$overlayDir" BIN="$base" "$base/test_gl"
    findTracks

    # test_pack tests the atlas API, which older revisions don't have.
    if [ -f "$base/src/AssAtlas.h" ]; then
        make -s "$BIN/test_pack"
        make -s OVERLAY_DIR="$overlayDir" BIN="$base" "$base/test_pack"
        findDumps
        echo "== test_pack: $rev, then the working tree"
        paste -d '\n' <(packChecks "$base" | sed 's/^/base: /') <(packChecks "$BIN" | sed 's/^/new:  /')
    fi

    echo "== test_gl: rendered frames of $rev vs the working tree"
    glChecks "$base" > "$base/gl.txt" || true
    glChecks "$BIN" > "$base/gl-new.txt" || true
    paste -d '\t' "$base/gl.txt" "$base/gl-new.txt" | while IFS=$'\t' read -r old new; do
        if [ "${old##* hash }" = "${new##* hash }" ]; then echo "same pixels:"; else echo "DIFFERENT PIXELS:"; fi
        echo "  base: ${old% hash *}"
        echo "  new:  ${new% hash *}"
    done
}

command=${1:-}
[ $# -gt 0 ] && shift
case "$command" in
prepare) prepare "$@" ;;
check)
    make -s "$BIN/test_pack"
    findDumps
    packChecks "$BIN"
    ;;
check-gl)
    make -s "$BIN/test_gl"
    findTracks
    glChecks "$BIN"
    ;;
bench)
    make -s "$BIN/bench_packers"
    findDumps
    for size in $MAX_SIZES; do "$BIN/bench_packers" "$size" "${DUMPS[@]}"; done
    ;;
stats)
    make -s "$BIN/frame_stats"
    findDumps
    "$BIN/frame_stats" "${DUMPS[@]}"
    ;;
compare) compare "$@" ;;
*) sed -n '2,12s/^# \{0,1\}//p' "$0" >&2; exit 2 ;;
esac
