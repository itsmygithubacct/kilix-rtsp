PROJECT := kilix-rtsp
BUILD_DIR ?= build
PREFIX ?= /usr/local
DESTDIR ?=

CC ?= cc
AR ?= ar
INSTALL ?= install

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude
WARNINGS := \
	-Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -fPIC $(WARNINGS) $(EXTRA_CFLAGS)
override LDFLAGS += $(EXTRA_LDFLAGS)
LDLIBS += -lpthread
DEPFLAGS := -MMD -MP

# Vendored, pinned dependencies for the terminal-facing commands.  The
# library itself (acquisition) depends on none of them.
KTS := third_party/kitty-terminal-session
KIN := $(KTS)/third_party/kitty-input
KKB := $(KIN)/third_party/kitty_keyboard
SR  := third_party/soft-raster
KPB := third_party/kitty-pty-broker

VIEW_CPPFLAGS := \
	-I$(KTS)/include -I$(KTS)/third_party/kitty-framebuffer/include \
	-I$(KIN)/include -I$(KKB)/include -I$(SR)/include -I$(KPB)/include -Isrc

# Our own terminal-facing sources: full warning set.
VIEW_SOURCES := \
	src/main.c \
	src/krtsp_probe.c \
	src/krtsp_view.c \
	src/krtsp_attach.c

# Vendored dependencies, compiled with the project's flags minus the
# noisiest conversion warnings.  They are pinned upstream code; letting
# their warnings through would bury our own.
VENDOR_SOURCES := \
	$(KPB)/src/kitty_pty_broker.c \
	$(KTS)/src/kitty_terminal_session.c \
	$(KTS)/third_party/kitty-framebuffer/src/kitty_framebuffer.c \
	$(KIN)/src/kitty_input.c \
	$(KIN)/src/kitty_input_posix.c \
	$(KKB)/src/kitty_keyboard.c \
	$(KKB)/src/kitty_keyboard_posix.c \
	$(SR)/src/soft_raster.c

VENDOR_CFLAGS := $(CFLAGS) -Wno-conversion -Wno-sign-conversion

# One object per vendored source, built in place.  Compiling them as a
# group into the current directory and moving the results afterwards put
# objects in the work tree, swept up any unrelated *.o next to them, and
# recompiled every dependency on every build.
VENDOR_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/vendor/%.o,$(notdir $(VENDOR_SOURCES)))
VIEW_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(VIEW_SOURCES))

VIEW_LDLIBS := -lz -lm

OBJECTS := \
	$(BUILD_DIR)/krtsp_args.o \
	$(BUILD_DIR)/krtsp_frame.o \
	$(BUILD_DIR)/krtsp_exec.o \
	$(BUILD_DIR)/krtsp_source.o \
	$(BUILD_DIR)/krtsp_paths.o \
	$(BUILD_DIR)/krtsp_config.o \
	$(BUILD_DIR)/krtsp_mosaic.o

STATIC_LIB := $(BUILD_DIR)/lib$(PROJECT).a
SHARED_LIB := $(BUILD_DIR)/lib$(PROJECT).so

TESTS := \
	$(BUILD_DIR)/test-args \
	$(BUILD_DIR)/test-frame \
	$(BUILD_DIR)/test-source \
	$(BUILD_DIR)/test-probe \
	$(BUILD_DIR)/test-config \
	$(BUILD_DIR)/test-mosaic

FAKE_FFMPEG := $(BUILD_DIR)/fake-ffmpeg

.DEFAULT_GOAL := all
.PHONY: all clean install test sanitize

all: $(STATIC_LIB) $(SHARED_LIB) $(BUILD_DIR)/kilix-rtsp

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: src/%.c include/kilix_rtsp.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(VIEW_OBJECTS): $(BUILD_DIR)/%.o: src/%.c include/kilix_rtsp.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(VIEW_CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(STATIC_LIB): $(OBJECTS)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(OBJECTS)
	$(CC) -shared $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/vendor:
	mkdir -p $@

# vpath lets one pattern rule cover vendored sources from several trees.
vpath %.c $(sort $(dir $(VENDOR_SOURCES)))

$(BUILD_DIR)/vendor/%.o: %.c | $(BUILD_DIR)/vendor
	$(CC) $(CPPFLAGS) $(VIEW_CPPFLAGS) $(VENDOR_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/kilix-rtsp: $(VIEW_OBJECTS) $(VENDOR_OBJECTS) \
		$(STATIC_LIB) | $(BUILD_DIR)
	@test -f $(KTS)/src/kitty_terminal_session.c || { \
		printf 'submodules missing; run: git submodule update --init --recursive\n' >&2; \
		exit 1; }
	$(CC) $(LDFLAGS) \
		$(VIEW_OBJECTS) $(VENDOR_OBJECTS) \
		$(STATIC_LIB) $(LDLIBS) $(VIEW_LDLIBS) -o $@

# A stand-in ffmpeg that can be told to misbehave, so process supervision
# is testable without a camera.  See tests/fake_ffmpeg.c.
$(FAKE_FFMPEG): tests/fake_ffmpeg.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) $< -o $@

$(BUILD_DIR)/test-%: tests/test_%.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -Isrc $< $(STATIC_LIB) \
		$(LDLIBS) -o $@

$(BUILD_DIR)/test-probe: tests/test_probe.c $(BUILD_DIR)/krtsp_probe.o \
		$(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -Isrc $< \
		$(BUILD_DIR)/krtsp_probe.o $(STATIC_LIB) $(LDLIBS) -o $@

test: $(TESTS) $(FAKE_FFMPEG) $(BUILD_DIR)/kilix-rtsp
	@set -e; for t in $(TESTS); do \
		printf '\n== %s ==\n' "$$t"; \
		KRTSP_FAKE_FFMPEG="$(abspath $(FAKE_FFMPEG))" "$$t"; \
	done
	@KRTSP_FAKE_FFMPEG="$(abspath $(FAKE_FFMPEG))" \
		sh tests/test_cli.sh "$(abspath $(BUILD_DIR)/kilix-rtsp)" \
		"$(abspath $(FAKE_FFMPEG))"
	@printf '\nall test suites passed\n'

SANITIZE_BUILD_DIR ?= $(BUILD_DIR)/sanitize
sanitize:
	@$(MAKE) --no-print-directory \
		BUILD_DIR="$(SANITIZE_BUILD_DIR)" \
		EXTRA_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
		EXTRA_LDFLAGS="-fsanitize=address,undefined" test

install: all $(BUILD_DIR)/kilix-rtsp
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/include
	$(INSTALL) -m 644 include/kilix_rtsp.h $(DESTDIR)$(PREFIX)/include/
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/lib
	$(INSTALL) -m 644 $(STATIC_LIB) $(DESTDIR)$(PREFIX)/lib/
	$(INSTALL) -m 644 $(SHARED_LIB) $(DESTDIR)$(PREFIX)/lib/
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m 755 $(BUILD_DIR)/kilix-rtsp $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJECTS:.o=.d) $(VIEW_OBJECTS:.o=.d) \
	$(VENDOR_OBJECTS:.o=.d) $(TESTS:=.d) $(FAKE_FFMPEG).d
