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
override CFLAGS += -std=c11 -fPIC $(WARNINGS)
LDLIBS += -lpthread

# Vendored, pinned dependencies for the terminal-facing commands.  The
# library itself (acquisition) depends on none of them.
KTS := third_party/kitty-terminal-session
KIN := $(KTS)/third_party/kitty-input
KKB := $(KIN)/third_party/kitty_keyboard
SR  := third_party/soft-raster

VIEW_CPPFLAGS := \
	-I$(KTS)/include -I$(KTS)/third_party/kitty-framebuffer/include \
	-I$(KIN)/include -I$(KKB)/include -I$(SR)/include -Isrc

VIEW_SOURCES := \
	src/krtsp_view.c \
	$(KTS)/src/kitty_terminal_session.c \
	$(KTS)/third_party/kitty-framebuffer/src/kitty_framebuffer.c \
	$(KIN)/src/kitty_input.c \
	$(KIN)/src/kitty_input_posix.c \
	$(KKB)/src/kitty_keyboard.c \
	$(KKB)/src/kitty_keyboard_posix.c \
	$(SR)/src/soft_raster.c

VIEW_LDLIBS := -lz -lm

OBJECTS := \
	$(BUILD_DIR)/krtsp_args.o \
	$(BUILD_DIR)/krtsp_frame.o \
	$(BUILD_DIR)/krtsp_source.o \
	$(BUILD_DIR)/krtsp_paths.o \
	$(BUILD_DIR)/krtsp_config.o

STATIC_LIB := $(BUILD_DIR)/lib$(PROJECT).a
SHARED_LIB := $(BUILD_DIR)/lib$(PROJECT).so

TESTS := \
	$(BUILD_DIR)/test-args \
	$(BUILD_DIR)/test-frame \
	$(BUILD_DIR)/test-source \
	$(BUILD_DIR)/test-config

FAKE_FFMPEG := $(BUILD_DIR)/fake-ffmpeg

.DEFAULT_GOAL := all
.PHONY: all clean install test sanitize

all: $(STATIC_LIB) $(SHARED_LIB) $(BUILD_DIR)/kilix-rtsp

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: src/%.c include/kilix_rtsp.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(OBJECTS)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(OBJECTS)
	$(CC) -shared $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/kilix-rtsp: src/main.c $(VIEW_SOURCES) $(STATIC_LIB) | $(BUILD_DIR)
	@test -f $(KTS)/src/kitty_terminal_session.c || { \
		printf 'submodules missing; run: git submodule update --init --recursive\n' >&2; \
		exit 1; }
	$(CC) $(CPPFLAGS) $(VIEW_CPPFLAGS) $(CFLAGS) $(LDFLAGS) \
		src/main.c $(VIEW_SOURCES) $(STATIC_LIB) $(LDLIBS) $(VIEW_LDLIBS) -o $@

# A stand-in ffmpeg that can be told to misbehave, so process supervision
# is testable without a camera.  See tests/fake_ffmpeg.c.
$(FAKE_FFMPEG): tests/fake_ffmpeg.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

$(BUILD_DIR)/test-%: tests/test_%.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $< $(STATIC_LIB) $(LDLIBS) -o $@

test: $(TESTS) $(FAKE_FFMPEG)
	@set -e; for t in $(TESTS); do printf '\n== %s ==\n' "$$t"; "$$t"; done
	@printf '\nall test suites passed\n'

sanitize: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: LDFLAGS += -fsanitize=address,undefined
sanitize: clean
	@$(MAKE) --no-print-directory \
		CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" test

install: all $(BUILD_DIR)/kilix-rtsp
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/include
	$(INSTALL) -m 644 include/kilix_rtsp.h $(DESTDIR)$(PREFIX)/include/
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/lib
	$(INSTALL) -m 644 $(STATIC_LIB) $(DESTDIR)$(PREFIX)/lib/
	$(INSTALL) -m 755 $(SHARED_LIB) $(DESTDIR)$(PREFIX)/lib/
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m 755 $(BUILD_DIR)/kilix-rtsp $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -rf $(BUILD_DIR)
