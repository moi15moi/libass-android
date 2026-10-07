# Host tools that test and benchmark the overlay on real libass output, off-device: see README.md.
# Needs a C compiler, pkg-config, libass, and EGL + GLES 3 (Mesa) headers and libraries.

CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
# The overlay sources the tools are built from; run.sh compare points it at an older revision's.
OVERLAY_DIR ?= $(abspath ../../main/cpp)
BIN ?= build/bin

LIBASS := $(shell pkg-config --cflags --libs libass)
GL := $(shell pkg-config --libs egl glesv2)
# The overlay compiled for the host: its Android and JNI headers are stubbed, GLES and libass are real.
OVERLAY := -Istubs -I$(OVERLAY_DIR)
# All of the overlay but AssKt.c, the JNI glue, which needs a JVM.
OVERLAY_SRCS = $(filter-out $(OVERLAY_DIR)/AssKt.c,$(wildcard $(OVERLAY_DIR)/*.c))
# The atlas layout alone, which involves no GL.
ATLAS_SRCS = $(OVERLAY_DIR)/AssAtlas.c $(OVERLAY_DIR)/AssSkyline.c
# The GL calls through which test_gl steers and watches the overlay.
GL_WRAPS := -Wl,--wrap=glGetIntegerv,--wrap=glGetString,--wrap=glDrawArrays,--wrap=glTexSubImage2D

all: $(BIN)/dump_frames $(BIN)/frame_stats $(BIN)/test_pack $(BIN)/test_gl $(BIN)/bench_packers

$(BIN)/dump_frames: dump_frames.c frame_dump.h libass_track.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< $(LIBASS)

$(BIN)/frame_stats: frame_stats.c frame_dump.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN)/test_pack $(BIN)/bench_packers: $(BIN)/%: %.c frame_dump.h $(ATLAS_SRCS) $(OVERLAY_DIR)/AssAtlas.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(OVERLAY) -o $@ $< $(ATLAS_SRCS) $(LIBASS)

$(BIN)/test_gl: test_gl.c libass_track.h $(OVERLAY_SRCS) $(wildcard $(OVERLAY_DIR)/*.h)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(OVERLAY) -o $@ $< $(OVERLAY_SRCS) $(LIBASS) $(GL) $(GL_WRAPS)

clean:
	rm -rf $(BIN)

.PHONY: all clean
