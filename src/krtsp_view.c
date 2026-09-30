/*
 * The view command: one camera filling the terminal.
 *
 * Frames arrive from ffmpeg already scaled and letterboxed to exactly the
 * framebuffer size, so the loop never scales anything - it borrows the
 * newest frame, optionally draws a status banner over it, and presents.
 *
 * Two decisions worth stating:
 *
 * A healthy fullscreen source is requested as RGBA and passed straight to
 * kittyts_present(), which copies it before returning.  No intermediate
 * full-frame conversion is needed on the path that runs almost all the time.
 * Only a degraded frame needs a private soft-raster canvas for its banner;
 * that rare path converts RGBA to 0xAARRGGBB, draws, then packs back to RGBA.
 * Mosaic tiles remain BGRA because every composite draws captions.
 *
 * The loop presents only when the frame sequence changes.  These cameras
 * deliver 8-20 fps; presenting the same frame at 60 would spend the whole
 * transport budget re-sending pixels that did not change.
 */

#include "kilix_rtsp.h"
#include "krtsp_view.h"
#include "krtsp_attach.h"
#include "krtsp_compose.h"
#include "krtsp_detect.h"
#include "krtsp_history.h"
#include "krtsp_source_internal.h"

#include "kitty_terminal_session.h"
#include "soft_raster.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* A fatal signal must restore the terminal, and a handler cannot carry a
 * pointer, so the active session is process-global for that path only. */
static kittyts_session *g_session;
static volatile sig_atomic_t g_quit;

static void handle_fatal(int signal_number)
{
    if (g_session != NULL) {
        kittyts_emergency_restore(g_session);
    }
    _exit(128 + signal_number);
}

static void handle_interrupt(int signal_number)
{
    (void)signal_number;
    g_quit = 1;
}

static long long monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (long long)now.tv_sec * 1000 + (long long)now.tv_nsec / 1000000;
}

static void sleep_ms(int milliseconds)
{
    struct timespec pause;

    if (milliseconds <= 0) {
        return;
    }
    pause.tv_sec = milliseconds / 1000;
    pause.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    (void)nanosleep(&pause, NULL);
}

static bool rgba_size(int width, int height, size_t *bytes)
{
    if (bytes == NULL || width <= 0 || height <= 0 ||
        (size_t)width > SIZE_MAX / 4u / (size_t)height) {
        return false;
    }
    *bytes = (size_t)width * (size_t)height * 4u;
    return true;
}

static void copy_rgba_to_canvas(uint32_t *canvas, const uint8_t *rgba,
                                size_t pixels)
{
    for (size_t index = 0u; index < pixels; ++index) {
        size_t at = index * 4u;

        canvas[index] = (uint32_t)rgba[at + 3u] << 24 |
                        (uint32_t)rgba[at] << 16 |
                        (uint32_t)rgba[at + 1u] << 8 |
                        (uint32_t)rgba[at + 2u];
    }
}

/*
 * Draw a status banner, and report the region it covered so a caller
 * can re-present just that.
 *
 * This is the most valuable thing on the screen.  A camera that froze an
 * hour ago keeps handing back a perfectly valid frame forever, and it is
 * indistinguishable from a quiet driveway until something says otherwise.
 */
static void draw_banner(sr_canvas *canvas, const char *text, uint32_t accent,
                        krtsp_damage *extent)
{
    const int scale = canvas->w >= 960 ? 2 : 1;
    int text_width = sr_text_width(text, scale);
    int pad = 6 * scale;
    int height = SR_FONT_H * scale + pad * 2;
    int width = text_width + pad * 2;

    if (width > canvas->w) {
        width = canvas->w;
    }
    if (extent != NULL) {
        extent->x0 = 0;
        extent->y0 = 0;
        extent->x1 = width;
        extent->y1 = height < canvas->h ? height : canvas->h;
    }
    /* A translucent plate keeps the text readable over a bright scene
     * without hiding the part of the picture that matters.  The fill
     * blends the same pixels a per-pixel loop would - integer-aligned
     * edges have coverage 1, and test_compose pins the equivalence. */
    sr_fill_rect(canvas, 0.0f, 0.0f, (float)width, (float)height,
                 0x000000u, 0.55f);
    sr_fill_rect(canvas, 0.0f, 0.0f, 3.0f * (float)scale, (float)height,
                 accent, 1.0f);
    sr_text_shadow(canvas, (float)pad, (float)pad, text, 0xFFFFFFu, 1.0f,
                   scale);
}

static void draw_centered_notice(sr_canvas *canvas, const char *text)
{
    int scale = canvas->w >= 960 ? 3 : 2;

    sr_clear(canvas, 0x101014u);
    sr_text_center(canvas, (float)canvas->w / 2.0f,
                   (float)canvas->h / 2.0f - (float)(SR_FONT_H * scale) / 2.0f,
                   text, 0xB0B0C0u, 1.0f, scale);
}

static uint32_t status_accent(krtsp_status status)
{
    switch (status) {
    case KRTSP_ONLINE:   return 0x40C060u;
    case KRTSP_STARTING: return 0xC0A040u;
    case KRTSP_STALE:    return 0xE08030u;
    default:             return 0xD04040u;
    }
}


/*
 * Bring up the terminal for a full-window video view.
 *
 * Shared by both commands: they differ in what they draw, not in how the
 * terminal is acquired, and two copies of this drifted apart once already
 * (the framebuffer size cap was raised in one and not the other).
 */
static bool start_terminal(kittyts_session *session, bool mouse)
{
    kittyts_options options;

    g_quit = 0;
    kittyts_options_init(&options);
    kittyts_session_init(session);
    /* AUTO picks shared memory when it can, which is what makes a
     * full-window camera view affordable at all. */
    options.framebuffer.transport = KITTYFB_TRANSPORT_AUTO;
    /*
     * Let the framebuffer use the whole terminal.  The library defaults
     * cap it at 1600x1000, which suits a game with a fixed art scale but
     * boxes a camera view inside a black border on a larger screen - and
     * letterboxes the picture a second time inside that box.
     */
    options.framebuffer.max_width = 7680;
    options.framebuffer.max_height = 4320;
    if (mouse) {
        /* Presses only: the view has one button, and motion reports would
         * be a stream of events for a pointer that is only ever passing. */
        options.mouse_tracking = KITTYIN_MOUSE_TRACKING_BUTTON;
        options.pixel_mouse = true;
    }

    if (kittyts_start(session, STDIN_FILENO, STDOUT_FILENO, &options) != 0) {
        if (errno == ENOTSUP) {
            (void)fprintf(stderr,
                "kilix-rtsp: this terminal answered the device query but does\n"
                "            not speak the Kitty graphics protocol.\n");
        } else {
            perror("kilix-rtsp: cannot start the terminal session");
        }
        return false;
    }
    g_session = session;
    (void)signal(SIGSEGV, handle_fatal);
    (void)signal(SIGBUS, handle_fatal);
    (void)signal(SIGABRT, handle_fatal);
    (void)signal(SIGINT, handle_interrupt);
    (void)signal(SIGTERM, handle_interrupt);
    (void)signal(SIGPIPE, SIG_IGN);
    return true;
}

/* Drain input and set g_quit on a quit key.  Both commands quit the same
 * way; only their drawing differs. */
static void consume_input(kittyts_session *session)
{
    kittyin_event event;

    (void)kittyts_read_input(session);
    while (kittyts_next_event(session, &event)) {
        if (event.kind == KITTYIN_EVENT_KEY &&
            event.data.key.action != KITTYKB_ACTION_RELEASE &&
            (event.data.key.key == 'q' || event.data.key.key == 'Q' ||
             event.data.key.key == 27u)) {
            g_quit = 1;
        }
    }
}

/* ------------------------------- the view -------------------------------- */

/* How long the help strip stays after a key or a click. */
#define HELP_MS 4000
/* A notice ("no detector installed") is read once, not lived with. */
#define NOTICE_MS 7000
/* A model at 20-100 ms a frame needs no more than this many offers a second
 * to keep boxes on a walking person, and every offer copies a frame. */
#define DETECT_INTERVAL_MS 200
/* Boxes for a frame that is this old are no longer where the thing is. */
#define DETECT_STALE_MS 2500
#define SEGMENT_TABLE_MAX 4096
#define SEGMENT_SECONDS 10

typedef struct view_input {
    krtsp_history_action history;
    bool toggle_detect;
    bool toggle_help;
    bool click;
    int click_x;
    int click_y;
    bool any;
} view_input;

static krtsp_history_action history_key(uint32_t key, uint32_t modifiers)
{
    bool shifted = (modifiers & KITTYKB_MOD_SHIFT) != 0u;

    switch (key) {
    case KITTYKB_KEY_LEFT:
        return shifted ? KRTSP_HISTORY_BACK_LONG : KRTSP_HISTORY_BACK_SHORT;
    case KITTYKB_KEY_RIGHT:
        return shifted ? KRTSP_HISTORY_FORWARD_LONG
                       : KRTSP_HISTORY_FORWARD_SHORT;
    case KITTYKB_KEY_PAGE_UP:
        return KRTSP_HISTORY_BACK_LONG;
    case KITTYKB_KEY_PAGE_DOWN:
        return KRTSP_HISTORY_FORWARD_LONG;
    case KITTYKB_KEY_HOME:
        return KRTSP_HISTORY_OLDEST;
    case KITTYKB_KEY_END:
        return KRTSP_HISTORY_LIVE_NOW;
    case ' ':
        return KRTSP_HISTORY_TOGGLE_PAUSE;
    default:
        return KRTSP_HISTORY_NONE;
    }
}

/*
 * Drain input for the view.  Any key or click counts as "the viewer is
 * here", which is what raises the help strip; the keys that mean something
 * are turned into actions and the rest do nothing but raise it.
 */
static void read_view_input(kittyts_session *session, view_input *out)
{
    kittyin_event event;

    (void)memset(out, 0, sizeof(*out));
    (void)kittyts_read_input(session);
    while (kittyts_next_event(session, &event)) {
        if (event.kind == KITTYIN_EVENT_KEY &&
            event.data.key.action != KITTYKB_ACTION_RELEASE) {
            uint32_t key = event.data.key.key;
            krtsp_history_action action;

            out->any = true;
            if (key == 'q' || key == 'Q' || key == 27u) {
                g_quit = 1;
                continue;
            }
            if (key == 'd' || key == 'D') {
                out->toggle_detect = true;
                continue;
            }
            if (key == 'h' || key == 'H' || key == '?' ||
                event.data.key.shifted_key == '?' ||
                key == KITTYKB_KEY_F1 ||
                (event.data.key.text_length > 0u &&
                 event.data.key.text[0] == '?')) {
                out->toggle_help = true;
                continue;
            }
            action = history_key(key, event.data.key.modifiers);
            if (action != KRTSP_HISTORY_NONE) {
                out->history = action;
            }
        } else if (event.kind == KITTYIN_EVENT_MOUSE &&
                   event.data.mouse.action == KITTYIN_MOUSE_PRESS &&
                   event.data.mouse.button == 1u) {
            out->any = true;
            out->click = true;
            out->click_x = event.data.mouse.x - kittyts_origin_x(session);
            out->click_y = event.data.mouse.y - kittyts_origin_y(session);
        }
    }
}

/* Everything the overlay needs to say, gathered so drawing decides nothing. */
typedef struct overlay_info {
    bool history_available;
    krtsp_history_mode mode;
    int behind;
    int span;                /* seconds in the buffer */
    uint64_t buffer_bytes;
    uint64_t buffer_max;
    bool at_oldest;
    bool help;
    bool pinned;
    bool detect_on;
    krtsp_detector_state detect_state;
    const char *detect_error;
    const char *notice;
    const krtsp_detection *boxes;
    size_t box_count;
} overlay_info;

typedef struct button_box {
    int x;
    int y;
    int width;
    int height;
    bool valid;
} button_box;

static uint32_t class_colour(int class_id)
{
    static const uint32_t palette[] = {
        0x40C060u, 0xE0A030u, 0x4090E0u, 0xD05050u,
        0xB060D0u, 0x30C0C0u, 0xD0D040u, 0xE07030u
    };

    return palette[(unsigned)(class_id < 0 ? 0 : class_id) %
                   (sizeof(palette) / sizeof(palette[0]))];
}

static void plate(sr_canvas *canvas, int x, int y, int width, int height,
                  float alpha)
{
    for (int row = y; row < y + height && row < canvas->h; ++row) {
        for (int column = x; column < x + width && column < canvas->w;
             ++column) {
            if (row >= 0 && column >= 0) {
                sr_blend(canvas, column, row, 0x000000u, alpha);
            }
        }
    }
}

static void draw_boxes(sr_canvas *canvas, const overlay_info *info,
                       int scale)
{
    for (size_t index = 0u; index < info->box_count; ++index) {
        const krtsp_detection *box = &info->boxes[index];
        uint32_t colour = class_colour(box->class_id);
        char label[64];
        int label_width;
        int label_height = SR_FONT_H * scale;
        int label_y;

        sr_stroke_rect(canvas, (float)box->x0, (float)box->y0,
                       (float)(box->x1 - box->x0), (float)(box->y1 - box->y0),
                       (float)(2 * scale), colour, 1.0f);
        (void)snprintf(label, sizeof(label), "%s %d%%",
                       krtsp_detect_class_name(box->class_id),
                       (int)(box->score * 100.0f + 0.5f));
        label_width = sr_text_width(label, scale) + 6 * scale;
        /* Above the box, or inside its top edge when there is no room. */
        label_y = box->y0 - label_height - 2 * scale;
        if (label_y < 0) {
            label_y = box->y0 + 2 * scale;
        }
        plate(canvas, box->x0, label_y, label_width, label_height + 2 * scale,
              0.65f);
        sr_fill_rect(canvas, (float)box->x0, (float)label_y,
                     (float)(2 * scale), (float)(label_height + 2 * scale),
                     colour, 1.0f);
        sr_text_shadow(canvas, (float)(box->x0 + 4 * scale),
                       (float)(label_y + scale), label, 0xFFFFFFu, 1.0f,
                       scale);
    }
}

/* The top-left pill: where in time this picture is from. */
static void draw_time_pill(sr_canvas *canvas, const overlay_info *info,
                           int scale)
{
    char text[160];
    char behind[24];
    char size[24];
    uint32_t accent;
    int pad = 4 * scale;
    int width;
    int height;

    if (!info->history_available) {
        return;
    }
    if (info->mode == KRTSP_HISTORY_LIVE) {
        /* Live is the default and says nothing until asked. */
        if (!info->help) {
            return;
        }
        krtsp_buffer_format_bytes(info->buffer_bytes, size, sizeof(size));
        krtsp_history_format(info->span, behind, sizeof(behind));
        (void)snprintf(text, sizeof(text), "LIVE  buffer %s  %s",
                       behind + 1, size);
        accent = 0x40C060u;
    } else {
        krtsp_history_format(info->behind, behind, sizeof(behind));
        (void)snprintf(text, sizeof(text), "%s  %s%s",
                       info->mode == KRTSP_HISTORY_PAUSED ? "PAUSED"
                                                          : "REPLAY",
                       behind, info->at_oldest ? "  start of buffer" : "");
        accent = info->mode == KRTSP_HISTORY_PAUSED ? 0xE0A030u : 0x4090E0u;
    }
    width = sr_text_width(text, scale) + pad * 2;
    height = SR_FONT_H * scale + pad * 2;
    plate(canvas, 0, 0, width, height, 0.6f);
    sr_fill_rect(canvas, 0.0f, 0.0f, (float)(3 * scale), (float)height,
                 accent, 1.0f);
    sr_text_shadow(canvas, (float)(pad + 2 * scale), (float)pad, text,
                   0xFFFFFFu, 1.0f, scale);
}

/* The top-right badge: whether detection is on, and if not, why not. */
static void draw_detect_badge(sr_canvas *canvas, const overlay_info *info,
                              int scale)
{
    char text[200];
    int pad = 4 * scale;
    int width;
    int height;
    uint32_t accent = 0x40C060u;

    if (info->notice != NULL && info->notice[0] != '\0') {
        (void)snprintf(text, sizeof(text), "%s", info->notice);
        accent = 0xE0A030u;
    } else if (info->detect_on) {
        if (info->detect_state == KRTSP_DETECTOR_FAILED) {
            (void)snprintf(text, sizeof(text), "DETECT failed: %s",
                           info->detect_error);
            accent = 0xD04040u;
        } else if (info->detect_state == KRTSP_DETECTOR_LOADING) {
            (void)snprintf(text, sizeof(text), "DETECT loading model...");
            accent = 0xC0A040u;
        } else {
            (void)snprintf(text, sizeof(text), "DETECT %zu",
                           info->box_count);
        }
    } else {
        return;
    }
    width = sr_text_width(text, scale) + pad * 2;
    if (width > canvas->w) {
        width = canvas->w;
    }
    height = SR_FONT_H * scale + pad * 2;
    plate(canvas, canvas->w - width, 0, width, height, 0.6f);
    sr_fill_rect(canvas, (float)(canvas->w - width), 0.0f,
                 (float)(3 * scale), (float)height, accent, 1.0f);
    sr_text_shadow(canvas, (float)(canvas->w - width + pad + 2 * scale),
                   (float)pad, text, 0xFFFFFFu, 1.0f, scale);
}

/* The one button, always there: a small chip in the bottom-right corner.
 * Hidden controls cannot be clicked, so it stays put and merely fades back
 * when nobody is using the view. */
static int chip_text(const overlay_info *info, char *out, size_t capacity)
{
    return snprintf(out, capacity, " [d] detect: %s ",
                    !info->detect_on ? "off"
                    : info->detect_state == KRTSP_DETECTOR_FAILED ? "failed"
                    : info->detect_state == KRTSP_DETECTOR_LOADING ? "loading"
                                                                   : "on");
}

static void draw_button(sr_canvas *canvas, const overlay_info *info,
                        int scale, button_box *button)
{
    char chip[48];
    int pad = 4 * scale;
    int height = SR_FONT_H * scale + pad * 2;
    int chip_width;
    int x;
    int y;
    int h;
    /* Dim when idle so a camera left on a wall shows the picture and not
     * a control; solid while the strip is up or detection is running. */
    bool quiet = !info->help && !info->detect_on;
    float alpha = quiet ? 0.55f : 0.95f;
    uint32_t fill = info->detect_on ? 0x2E7D46u : 0x3A3A44u;

    (void)chip_text(info, chip, sizeof(chip));
    chip_width = sr_text_width(chip, scale) + 2 * scale;
    x = canvas->w - chip_width - pad;
    y = canvas->h - height + scale;
    h = height - 2 * scale;
    sr_fill_rect(canvas, (float)x, (float)y, (float)chip_width, (float)h,
                 fill, alpha);
    sr_stroke_rect(canvas, (float)x, (float)y, (float)chip_width, (float)h,
                   (float)scale, info->detect_on ? 0x6FE09Au : 0x8A8A99u,
                   alpha);
    sr_text(canvas, (float)(x + scale), (float)(canvas->h - height + pad),
            chip, 0xFFFFFFu, alpha, scale);
    button->x = x;
    button->y = y;
    button->width = chip_width;
    button->height = h;
    button->valid = true;
}

/*
 * The strip along the bottom: the keys.  It is drawn only while somebody is
 * using the view, and it is low: a single line of small text on a
 * translucent plate that leaves the picture above it alone.
 */
static void draw_help_strip(sr_canvas *canvas, const overlay_info *info,
                            int scale)
{
    static const char *const with_history[] = {
        "Left/Right 10s   PgUp/PgDn 60s   Home start   End live   "
        "Space pause   ? help   q quit",
        "Left/Right 10s   Home/End   Space pause   q quit",
        "Left/Right   End live   q"
    };
    static const char *const without_history[] = {
        "history off   ? help   q quit",
        "q quit"
    };
    const char *const *tiers = info->history_available ? with_history
                                                       : without_history;
    size_t tier_count = info->history_available ? 3u : 2u;
    char chip[48];
    const char *text = tiers[tier_count - 1u];
    int pad = 4 * scale;
    int height = SR_FONT_H * scale + pad * 2;
    int top = canvas->h - height;
    int room;

    (void)chip_text(info, chip, sizeof(chip));
    room = canvas->w - (sr_text_width(chip, scale) + 2 * scale) - pad * 3;
    for (size_t index = 0u; index < tier_count; ++index) {
        if (sr_text_width(tiers[index], scale) <= room) {
            text = tiers[index];
            break;
        }
    }
    plate(canvas, 0, top, canvas->w, height, 0.6f);
    sr_text_shadow(canvas, (float)pad, (float)(top + pad), text, 0xE0E0E0u,
                   1.0f, scale);
}

/* Small unless the picture is very large: everything drawn here is a
 * caption on somebody else's picture. */
static int overlay_scale(int width)
{
    return width >= 1900 ? 2 : 1;
}

static void draw_overlay(sr_canvas *canvas, const overlay_info *info,
                         button_box *button)
{
    int scale = overlay_scale(canvas->w);

    button->valid = false;
    draw_boxes(canvas, info, scale);
    draw_time_pill(canvas, info, scale);
    draw_detect_badge(canvas, info, scale);
    if (info->help) {
        draw_help_strip(canvas, info, scale);
    }
    draw_button(canvas, info, scale, button);
}

static bool in_button(const button_box *button, int x, int y)
{
    return button->valid && x >= button->x && x < button->x + button->width &&
           y >= button->y && y < button->y + button->height;
}

/*
 * Whether there is a live stream to keep a history of.  An ordinary file is
 * already its own history - a buffer of a recording would copy what is on
 * disk already - so it is the one thing refused; a camera, a UDP feed and a
 * pipe are not.
 */
static bool is_live_source(const char *url)
{
    struct stat info;

    return !(stat(url, &info) == 0 && S_ISREG(info.st_mode));
}

static bool segment_path(const char *dir, const krtsp_segment *segment,
                         char *out, size_t capacity)
{
    return snprintf(out, capacity, "%s/%s", dir, segment->name) <
           (int)capacity;
}

/* The view's mutable world, so the loop reads as decisions and not as a
 * page of locals. */
typedef struct view_state {
    krtsp_history history;
    krtsp_buffer_plan plan;
    char buffer_dir[PATH_MAX];
    bool buffering;
    krtsp_segment *segments;
    size_t segment_count;
    uint64_t buffer_bytes;
    long long scanned_at;

    krtsp_source *replay;            /* a segment being played, or NULL */
    char replay_name[KRTSP_SEGMENT_NAME_MAX];
    bool replay_dirty;               /* the position jumped: reopen */

    uint8_t *frozen;                 /* the frame a pause holds */
    bool frozen_valid;

    krtsp_detector *detector;
    bool detect_on;
    long long detect_submitted_at;
    uint64_t detect_generation;
    long long detect_answered_at;
    krtsp_detection boxes[KRTSP_DETECT_MAX];
    size_t box_count;
    char notice[160];
    long long notice_until;
} view_state;

static void set_notice(view_state *view, long long now_ms, const char *text)
{
    (void)snprintf(view->notice, sizeof(view->notice), "%s", text);
    view->notice_until = now_ms + NOTICE_MS;
}

static void rescan(view_state *view, time_t now)
{
    view->segment_count = 0u;
    view->buffer_bytes = 0u;
    if (!view->buffering) {
        return;
    }
    (void)krtsp_segments_prune(view->buffer_dir, &view->plan, now, 2u);
    view->segment_count = krtsp_segments_scan(
        view->buffer_dir, view->segments, SEGMENT_TABLE_MAX);
    for (size_t index = 0u; index < view->segment_count; ++index) {
        view->buffer_bytes += view->segments[index].bytes;
    }
}

static time_t oldest_start(const view_state *view)
{
    return view->segment_count == 0u ? 0 : view->segments[0].start;
}

static void stop_replay(view_state *view)
{
    krtsp_source_stop(view->replay);
    view->replay = NULL;
    view->replay_name[0] = '\0';
}

static bool start_replay(view_state *view, const krtsp_source_options *live,
                         const krtsp_segment *segment, int offset)
{
    krtsp_source_options options;
    char path[PATH_MAX];

    stop_replay(view);
    if (!segment_path(view->buffer_dir, segment, path, sizeof(path))) {
        return false;
    }
    krtsp_source_options_init(&options);
    options.width = live->width;
    options.height = live->height;
    options.fps_cap = live->fps_cap;
    options.letterbox = true;
    options.pixfmt = KRTSP_PIXFMT_RGBA;
    /* Paced like the camera it came from, so replay runs at the speed it
     * was recorded, and opened at the moment rather than the segment's
     * start: see seek_seconds. */
    options.realtime = true;
    options.seek_seconds = offset;
    if (!krtsp_source_start(&view->replay, path, &options)) {
        view->replay = NULL;
        return false;
    }
    (void)snprintf(view->replay_name, sizeof(view->replay_name), "%s",
                   segment->name);
    return true;
}

/*
 * Make the playback source match the history's position: nothing when live,
 * the right segment at the right offset when replaying, and, when paused,
 * just long enough to fetch the frame that is held.
 */
static void sync_replay(view_state *view, const krtsp_source_options *live,
                        time_t now)
{
    krtsp_history *history = &view->history;
    size_t index = 0u;
    int offset = 0;

    if (history->mode == KRTSP_HISTORY_LIVE) {
        stop_replay(view);
        return;
    }
    if (!krtsp_segments_locate(view->segments, view->segment_count,
                               krtsp_history_position(history, now), &index,
                               &offset) &&
        view->segment_count == 0u) {
        stop_replay(view);
        krtsp_history_apply(history, KRTSP_HISTORY_LIVE_NOW, now, 0);
        return;
    }
    if (history->mode == KRTSP_HISTORY_PAUSED) {
        if (view->frozen_valid && !view->replay_dirty) {
            stop_replay(view);
            return;
        }
        if (view->replay == NULL || view->replay_dirty) {
            (void)start_replay(view, live, &view->segments[index], offset);
            view->replay_dirty = false;
        }
        return;
    }
    if (view->replay == NULL || view->replay_dirty ||
        strcmp(view->replay_name, view->segments[index].name) != 0) {
        (void)start_replay(view, live, &view->segments[index], offset);
        view->replay_dirty = false;
    }
}

static const char *detector_notice(void)
{
    return "no detector installed - run: kilix yolox install";
}

static void toggle_detect(view_state *view, long long now_ms)
{
    if (view->detect_on) {
        krtsp_detector_stop(view->detector);
        view->detector = NULL;
        view->detect_on = false;
        view->box_count = 0u;
        return;
    }
    {
        char storage[1024];
        const char *argv[8];

        if (krtsp_detect_resolve(storage, sizeof(storage), argv, 8u) == 0u) {
            set_notice(view, now_ms, detector_notice());
            return;
        }
        if (!krtsp_detector_start(&view->detector, argv)) {
            set_notice(view, now_ms, "could not start the detector");
            return;
        }
        view->detect_on = true;
        view->detect_generation = 0u;
        view->detect_submitted_at = 0;
        view->detect_answered_at = now_ms;
        view->box_count = 0u;
    }
}

int krtsp_view_run(const char *url, const char *label,
                   const krtsp_view_options *options)
{
    kittyts_session session;
    krtsp_source *source = NULL;
    krtsp_source_options source_options;
    view_state view;
    view_input input;
    overlay_info info;
    button_box button = {0, 0, 0, 0, false};
    sr_canvas canvas;
    krtsp_attach attach;
    bool streaming = true;
    long long attach_checked_at = 0;
    long long detached_since = 0;
    const long long detached_limit = krtsp_attach_detached_exit_ms();
    uint8_t *present_buffer = NULL;
    uint64_t last_sequence = UINT64_MAX;
    uint64_t last_replay_sequence = UINT64_MAX;
    krtsp_damage banner_shown = {0, 0, 0, 0};
    bool banner_on_screen = false;
    int notice_shown = -1;
    long long resize_pending_at = 0;
    long long status_presented_at = 0;
    long long help_until = 0;
    bool help_pinned = false;
    bool overlay_dirty = true;
    bool was_help = false;
    int fps_cap = options != NULL ? options->fps_cap : 0;
    int width;
    int height;
    int exit_code = 0;
    size_t present_size;

    if (url == NULL || label == NULL) {
        return 2;
    }
    (void)memset(&view, 0, sizeof(view));
    krtsp_history_init(&view.history);

    if (!start_terminal(&session, true)) {
        return 1;
    }

    width = kittyts_width(&session);
    height = kittyts_height(&session);
    if (!rgba_size(width, height, &present_size)) {
        kittyts_stop(&session);
        g_session = NULL;
        return 1;
    }
    krtsp_attach_init(&attach);

    krtsp_source_options_init(&source_options);
    source_options.width = width;
    source_options.height = height;
    source_options.fps_cap = fps_cap;
    /* ffmpeg letterboxes, so every frame is exactly framebuffer-sized and
     * the loop never scales. */
    source_options.letterbox = true;
    source_options.pixfmt = KRTSP_PIXFMT_RGBA;

    /*
     * The history buffer rides on the same ffmpeg session as the picture:
     * the camera sees one client, the segments are the camera's own
     * bitstream copied to disk, and nothing is re-encoded.
     */
    view.segments = calloc(SEGMENT_TABLE_MAX, sizeof(*view.segments));
    if (view.segments != NULL && is_live_source(url) && options != NULL &&
        options->buffer.kind != KRTSP_BUFFER_OFF) {
        uint64_t free_bytes = 0u;
        uint64_t total_bytes = 0u;

        (void)krtsp_buffer_sweep();
        if (krtsp_buffer_dir_make(label, view.buffer_dir,
                                  sizeof(view.buffer_dir)) &&
            krtsp_buffer_disk(view.buffer_dir, &free_bytes, &total_bytes)) {
            krtsp_buffer_plan_make(&options->buffer, free_bytes, total_bytes,
                                   &view.plan);
            if (view.plan.enabled) {
                view.buffering = true;
                source_options.roles = KRTSP_ROLE_DECODE | KRTSP_ROLE_RECORD;
                source_options.record_dir = view.buffer_dir;
                source_options.segment_seconds = SEGMENT_SECONDS;
            } else {
                krtsp_buffer_dir_remove(view.buffer_dir);
                view.buffer_dir[0] = '\0';
            }
        }
    }

    if (!krtsp_source_start(&source, url, &source_options)) {
        krtsp_buffer_dir_remove(view.buffer_dir);
        free(view.segments);
        kittyts_stop(&session);
        g_session = NULL;
        (void)fprintf(stderr, "kilix-rtsp: cannot start the source\n");
        return 1;
    }

    present_buffer = malloc(present_size);
    view.frozen = malloc(present_size);
    if (present_buffer == NULL || view.frozen == NULL) {
        free(present_buffer);
        free(view.frozen);
        krtsp_source_stop(source);
        krtsp_buffer_dir_remove(view.buffer_dir);
        free(view.segments);
        kittyts_stop(&session);
        g_session = NULL;
        return 1;
    }
    if (options != NULL && options->detect) {
        toggle_detect(&view, monotonic_ms());
    }

    while (!g_quit) {
        int new_width;
        int new_height;
        int age_ms = 0;
        const uint8_t *pixels = NULL;
        krtsp_source *shown = NULL;
        krtsp_status status;
        char banner[192];
        uint64_t sequence = 0u;
        long long now_ms = monotonic_ms();
        time_t now = time(NULL);
        bool replaying;
        bool have_frame;
        bool forced;
        krtsp_damage banner_rect = {0, 0, 0, 0};

        /*
         * Stop decoding while nobody is looking.
         *
         * Closing a kilix tab detaches the pane rather than ending it -
         * kitty-pty-broker keeps the session alive so it can be attached
         * again.  Nothing about the terminal reveals that: no SIGHUP,
         * writes still succeed, the window size still reads back.  So ask
         * the broker, and when the answer is "nobody", stop the stream
         * outright.  Decoding H.264 at a third of a core into a journal
         * no one is reading is pure waste, and a detached view is
         * invisible - there is nothing on screen to suggest it is still
         * running.
         */
        if (now_ms - attach_checked_at > 1000) {
            bool attached = krtsp_attach_is_attached(&attach);

            attach_checked_at = now_ms;
            if (attached) {
                detached_since = 0;
            } else if (detached_since == 0) {
                detached_since = now_ms;
            } else if (detached_limit > 0 &&
                       now_ms - detached_since >= detached_limit) {
                /* Nobody came back: end the view and its broker session. */
                exit_code = 0;
                break;
            }
            if (!attached && streaming) {
                krtsp_source_stop(source);
                source = NULL;
                stop_replay(&view);
                streaming = false;
            } else if (attached && !streaming) {
                if (!krtsp_source_start(&source, url, &source_options)) {
                    exit_code = 1;
                    break;
                }
                streaming = true;
                last_sequence = UINT64_MAX;
                view.replay_dirty = true;
                banner_on_screen = false;
                notice_shown = -1;
            }
        }
        if (!streaming) {
            /* Detached: no decode, no present, no busy loop.  The next
             * attach restarts the stream. */
            sleep_ms(250);
            continue;
        }

        /*
         * A resize means a new decode size, which means restarting
         * ffmpeg.  Dragging a window emits dozens of SIGWINCHes, so wait
         * for the size to settle first; in the meantime the old frames
         * keep being presented at the old size, which the framebuffer
         * handles by re-centring.
         */
        if (kittyts_check_resize(&session, &new_width, &new_height)) {
            resize_pending_at = now_ms;
            /* Whatever the resize leaves on screen, the notice is not
             * reliably it any more. */
            notice_shown = -1;
        }
        if (resize_pending_at != 0 && now_ms - resize_pending_at > 250) {
            resize_pending_at = 0;
            new_width = kittyts_width(&session);
            new_height = kittyts_height(&session);
            if (new_width != width || new_height != height) {
                size_t new_size;
                uint8_t *grown = NULL;
                uint8_t *grown_frozen = NULL;

                if (rgba_size(new_width, new_height, &new_size)) {
                    grown = realloc(present_buffer, new_size);
                    if (grown != NULL) {
                        present_buffer = grown;
                        grown_frozen = realloc(view.frozen, new_size);
                    }
                }

                if (grown != NULL && grown_frozen != NULL) {
                    view.frozen = grown_frozen;
                    view.frozen_valid = false;
                    present_size = new_size;
                    width = new_width;
                    height = new_height;
                    krtsp_source_stop(source);
                    source = NULL;
                    stop_replay(&view);
                    view.replay_dirty = true;
                    view.box_count = 0u;
                    source_options.width = width;
                    source_options.height = height;
                    if (!krtsp_source_start(&source, url, &source_options)) {
                        exit_code = 1;
                        break;
                    }
                    last_sequence = UINT64_MAX;
                    overlay_dirty = true;
                    banner_on_screen = false;
                    /* The resize scheduled a clear, so whatever was on
                     * screen - the notice included - must be repainted. */
                    notice_shown = -1;
                }
            }
        }

        read_view_input(&session, &input);
        if (g_quit) {
            break;
        }
        if (input.any) {
            help_until = now_ms + HELP_MS;
            overlay_dirty = true;
        }
        if (input.toggle_help) {
            help_pinned = !help_pinned;
        }
        if (input.click && in_button(&button, input.click_x, input.click_y)) {
            input.toggle_detect = true;
        }
        if (input.toggle_detect) {
            toggle_detect(&view, now_ms);
            overlay_dirty = true;
        }
        if (view.notice_until != 0 && now_ms >= view.notice_until) {
            view.notice[0] = '\0';
            view.notice_until = 0;
            overlay_dirty = true;
        }

        /* Keep the buffer's picture of itself current, about once a
         * second: that is also when retention runs. */
        if (view.buffering && now_ms - view.scanned_at >= 1000) {
            view.scanned_at = now_ms;
            rescan(&view, now);
            if (krtsp_history_clamp(&view.history, now,
                                    oldest_start(&view))) {
                view.replay_dirty = true;
                view.frozen_valid = false;
            }
        }

        if (input.history != KRTSP_HISTORY_NONE) {
            if (!view.buffering) {
                set_notice(&view, now_ms,
                           options != NULL &&
                                   options->buffer.kind == KRTSP_BUFFER_OFF
                               ? "history is off (--buffer)"
                               : is_live_source(url)
                                     ? "no room for a history buffer"
                                     : "history needs a live stream");
            } else {
                krtsp_history_mode before = view.history.mode;
                const uint8_t *hold = NULL;
                int hold_age = 0;

                /* A pause holds the frame on screen at that moment. */
                if (input.history == KRTSP_HISTORY_TOGGLE_PAUSE &&
                    before != KRTSP_HISTORY_PAUSED) {
                    krtsp_source *from = before == KRTSP_HISTORY_LIVE
                                             ? source : view.replay;

                    hold = krtsp_source_borrow_latest(from, NULL, &hold_age);
                    if (hold != NULL) {
                        (void)memcpy(view.frozen, hold, present_size);
                        view.frozen_valid = true;
                        krtsp_source_release(from);
                    }
                }
                if (krtsp_history_apply(&view.history, input.history, now,
                                        oldest_start(&view))) {
                    if (input.history != KRTSP_HISTORY_TOGGLE_PAUSE) {
                        /* A jump: the held frame and the open segment are
                         * both for a moment that is no longer this one. */
                        view.replay_dirty = true;
                        view.frozen_valid = false;
                    } else if (view.history.mode == KRTSP_HISTORY_REPLAY) {
                        /* Resuming: play on from where the pause was. */
                        view.replay_dirty = true;
                    }
                    last_replay_sequence = UINT64_MAX;
                    last_sequence = UINT64_MAX;
                }
                overlay_dirty = true;
            }
        }
        if (view.buffering) {
            sync_replay(&view, &source_options, now);
        }

        replaying = view.history.mode != KRTSP_HISTORY_LIVE;
        have_frame = false;
        if (view.history.mode == KRTSP_HISTORY_PAUSED) {
            /* A held frame; fetch it from the reopened segment if a step
             * moved the position, then let the source go. */
            if (!view.frozen_valid && view.replay != NULL) {
                const uint8_t *fetched = krtsp_source_borrow_latest(
                    view.replay, &sequence, &age_ms);

                if (fetched != NULL) {
                    (void)memcpy(view.frozen, fetched, present_size);
                    view.frozen_valid = true;
                    krtsp_source_release(view.replay);
                    stop_replay(&view);
                    overlay_dirty = true;
                }
            }
            if (view.frozen_valid) {
                pixels = view.frozen;
                have_frame = true;
                /* Nothing new arrives; only the overlay redraws, which
                 * overlay_dirty already says. */
                sequence = last_replay_sequence;
            }
            shown = NULL;
        } else if (replaying && view.replay != NULL) {
            pixels = krtsp_source_borrow_latest(view.replay, &sequence,
                                                &age_ms);
            shown = view.replay;
            have_frame = pixels != NULL;
        } else if (!replaying) {
            pixels = krtsp_source_borrow_latest(source, &sequence, &age_ms);
            shown = source;
            have_frame = pixels != NULL;
        }

        status = krtsp_source_status(source);

        if (!have_frame) {
            /* Nothing yet.  Say what is happening rather than showing a
             * black screen: a camera can take several seconds to produce
             * its first frame, and silence looks like a failure.
             *
             * Between status changes the notice is byte-identical -
             * nothing in it ages - so present it once per status and
             * then only keep polling.  A view left pointed at an
             * offline camera would otherwise push five identical
             * full-canvas frames a second through the transport,
             * indefinitely, to display nothing new. */
            int notice_key = replaying ? 1000 : (int)status;

            if (notice_key != notice_shown) {
                sr_canvas_wrap(&canvas, (uint32_t *)(void *)present_buffer,
                               width, height);
                if (replaying) {
                    (void)snprintf(banner, sizeof(banner),
                                   "%s: opening history", label);
                } else {
                    (void)snprintf(banner, sizeof(banner), "%s: %s", label,
                                   status == KRTSP_STARTING
                                       ? "connecting"
                                       : krtsp_status_name(status));
                }
                draw_centered_notice(&canvas, banner);
                (void)sr_pack_rgba(&canvas, present_buffer, present_size);
                if (!kittyts_present(&session, present_buffer, width,
                                     height)) {
                    exit_code = 1;
                    break;
                }
                /* The notice covers the canvas, banner included. */
                banner_on_screen = false;
                notice_shown = notice_key;
            }
            sleep_ms(200);
            continue;
        }
        notice_shown = -1;

        {
            bool live_degraded = !replaying &&
                                 (status != KRTSP_ONLINE || age_ms > 2000);
            bool help_visible = help_pinned || now_ms < help_until;
            bool changed;
            uint64_t *tracked = replaying ? &last_replay_sequence
                                          : &last_sequence;
            bool wants_detect;

            /* Boxes: offer the frame being shown to the detector, and
             * take whatever it has answered. */
            wants_detect = view.detect_on && view.detector != NULL;
            if (wants_detect) {
                uint64_t generation = 0u;

                if (now_ms - view.detect_submitted_at >= DETECT_INTERVAL_MS &&
                    krtsp_detector_submit(view.detector, pixels, width,
                                          height)) {
                    view.detect_submitted_at = now_ms;
                }
                view.box_count = krtsp_detector_take(
                    view.detector, view.boxes, KRTSP_DETECT_MAX, &generation);
                if (generation != view.detect_generation) {
                    view.detect_generation = generation;
                    view.detect_answered_at = now_ms;
                    overlay_dirty = true;
                }
                if (view.box_count != 0u &&
                    now_ms - view.detect_answered_at > DETECT_STALE_MS) {
                    view.box_count = 0u;
                    overlay_dirty = true;
                }
                if (krtsp_detector_status(view.detector) ==
                    KRTSP_DETECTOR_FAILED) {
                    overlay_dirty = true;
                }
            }
            if (help_visible != was_help) {
                was_help = help_visible;
                overlay_dirty = true;
            }

            forced = overlay_dirty;
            changed = sequence != *tracked;
            if (!changed && !live_degraded && !forced) {
                if (shown != NULL) {
                    krtsp_source_release(shown);
                }
                sleep_ms(8);
                continue;
            }
            if (!changed && live_degraded && !forced &&
                now_ms - status_presented_at < 250) {
                if (shown != NULL) {
                    krtsp_source_release(shown);
                }
                sleep_ms(8);
                continue;
            }
            *tracked = sequence;
            overlay_dirty = false;

            (void)memset(&info, 0, sizeof(info));
            info.history_available = view.buffering;
            info.mode = view.history.mode;
            info.behind = krtsp_history_behind(&view.history, now);
            info.span = view.segment_count == 0u
                            ? 0 : (int)(now - oldest_start(&view));
            info.buffer_bytes = view.buffer_bytes;
            info.buffer_max = view.plan.max_bytes;
            info.at_oldest = view.history.at_oldest &&
                             view.history.mode != KRTSP_HISTORY_LIVE;
            info.help = help_visible;
            info.pinned = help_pinned;
            info.detect_on = view.detect_on;
            info.detect_state = view.detector != NULL
                                    ? krtsp_detector_status(view.detector)
                                    : KRTSP_DETECTOR_LOADING;
            info.detect_error = view.detector != NULL
                                    ? krtsp_detector_error(view.detector) : "";
            info.notice = view.notice;
            info.boxes = view.boxes;
            info.box_count = view.box_count;

            {
                bool anything_to_draw =
                    live_degraded || replaying || info.help ||
                    info.detect_on || info.notice[0] != '\0';

                if (!anything_to_draw) {
                    /*
                     * The path that runs almost all the time.  The only
                     * thing on screen is the idle button, which lives in
                     * the last few rows, so only those are converted and
                     * drawn on; the rest of the frame goes to the terminal
                     * exactly as it came.
                     */
                    int scale = overlay_scale(width);
                    int rows = SR_FONT_H * scale + 8 * scale;
                    bool presented;

                    if (rows > height) {
                        rows = height;
                    }
                    (void)memcpy(present_buffer, pixels, present_size);
                    if (shown != NULL) {
                        krtsp_source_release(shown);
                    }
                    {
                        size_t first = (size_t)(height - rows) * (size_t)width;
                        uint32_t *tail =
                            (uint32_t *)(void *)present_buffer + first;

                        copy_rgba_to_canvas(tail, (const uint8_t *)tail,
                                            (size_t)rows * (size_t)width);
                        sr_canvas_wrap(&canvas, tail, width, rows);
                        button.valid = false;
                        draw_button(&canvas, &info, scale, &button);
                        button.y += height - rows;
                        (void)sr_pack_rgba(&canvas, present_buffer + first * 4u,
                                           (size_t)rows * (size_t)width * 4u);
                    }
                    presented = kittyts_present(&session, present_buffer,
                                                width, height);
                    banner_on_screen = false;
                    if (!presented) {
                        exit_code = 1;
                        break;
                    }
                    sleep_ms(8);
                    continue;
                }
            }

            /* Drawing over the frame needs a private copy: the borrowed
             * buffer belongs to the source. */
            copy_rgba_to_canvas((uint32_t *)(void *)present_buffer, pixels,
                                present_size / 4u);
            if (shown != NULL) {
                krtsp_source_release(shown);
            }
            sr_canvas_wrap(&canvas, (uint32_t *)(void *)present_buffer,
                           width, height);
            if (live_degraded) {
                (void)snprintf(banner, sizeof(banner), "%s  %s  %.1fs old",
                               label, krtsp_status_name(status),
                               (double)age_ms / 1000.0);
                draw_banner(&canvas, banner, status_accent(status),
                            &banner_rect);
                status_presented_at = now_ms;
            }
            draw_overlay(&canvas, &info, &button);
            (void)sr_pack_rgba(&canvas, present_buffer, present_size);
            if (live_degraded && !changed && !forced && !replaying &&
                !info.help && !info.detect_on && info.notice[0] == '\0') {
                /* The picture on screen is this same frozen frame; only
                 * the banner's age is new.  Patch the banner region
                 * instead of resending the frame - together with the
                 * previous banner's region, so a shorter banner erases
                 * the overhang of a longer one.  The presenter falls
                 * back to a full frame by itself whenever patching
                 * cannot help. */
                kittyfb_rect rects[2];
                size_t rect_count = 1u;
                bool presented;

                rects[0].x0 = banner_rect.x0;
                rects[0].y0 = banner_rect.y0;
                rects[0].x1 = banner_rect.x1;
                rects[0].y1 = banner_rect.y1;
                if (banner_on_screen) {
                    rects[1].x0 = banner_shown.x0;
                    rects[1].y0 = banner_shown.y0;
                    rects[1].x1 = banner_shown.x1;
                    rects[1].y1 = banner_shown.y1;
                    rect_count = 2u;
                }
                presented = kittyts_present_damage(
                    &session, present_buffer, width, height, rects,
                    rect_count);
                if (!presented) {
                    exit_code = 1;
                    break;
                }
            } else if (!kittyts_present(&session, present_buffer, width,
                                        height)) {
                exit_code = 1;
                break;
            }
            if (live_degraded) {
                banner_shown = banner_rect;
                banner_on_screen = true;
            } else {
                banner_on_screen = false;
            }
        }
        sleep_ms(8);
    }

    krtsp_detector_stop(view.detector);
    stop_replay(&view);
    free(present_buffer);
    free(view.frozen);
    krtsp_source_stop(source);
    krtsp_buffer_dir_remove(view.buffer_dir);
    free(view.segments);
    kittyts_stop(&session);
    g_session = NULL;
    return exit_code;
}

/* ------------------------------- mosaic --------------------------------- */

#define KRTSP_MOSAIC_MAX 16

/* Upper bound on full-canvas recomposites per second.  Cameras here
 * deliver 8-20 fps each and arrive independently. */
#define KRTSP_MOSAIC_MAX_FPS 20

typedef struct mosaic_slot {
    krtsp_source *source;
    krtsp_tile tile;
    const char *label;
    uint64_t last_frames;
    krtsp_status last_status;
} mosaic_slot;

/* Start one source per tile, each decoding straight to its tile size.
 * That is the whole reason a mosaic is affordable: cost follows output
 * pixels, and seven tiles hold fewer pixels than one full-screen view. */
static bool mosaic_start_sources(
    mosaic_slot *slots, size_t count, const char **urls, int fps_cap)
{
    krtsp_source_options options;

    for (size_t index = 0u; index < count; ++index) {
        krtsp_source_options_init(&options);
        options.width = slots[index].tile.width;
        options.height = slots[index].tile.height;
        options.fps_cap = fps_cap;
        options.letterbox = true;
        options.pixfmt = KRTSP_PIXFMT_BGRA;
        /* Sub stream: a tile is far smaller than the main stream, so the
         * extra resolution would be decoded only to be thrown away. */
        if (!krtsp_source_start(&slots[index].source, urls[index], &options)) {
            return false;
        }
        slots[index].last_frames = UINT64_MAX;
    }
    return true;
}

static void mosaic_stop_sources(mosaic_slot *slots, size_t count)
{
    for (size_t index = 0u; index < count; ++index) {
        krtsp_source_stop(slots[index].source);
        slots[index].source = NULL;
    }
}

int krtsp_mosaic_run(
    const char **urls, const char **labels, size_t count, int fps_cap)
{
    kittyts_session session;
    mosaic_slot slots[KRTSP_MOSAIC_MAX];
    krtsp_tile tiles[KRTSP_MOSAIC_MAX];
    krtsp_attach attach;
    krtsp_compositor compositor = {0};
    uint8_t *present_buffer = NULL;
    long long attach_checked_at = 0;
    long long detached_since = 0;
    const long long detached_limit = krtsp_attach_detached_exit_ms();
    long long resize_pending_at = 0;
    long long composed_at = 0;
    bool streaming = true;
    int width;
    int height;
    int exit_code = 0;
    size_t present_size;

    if (urls == NULL || labels == NULL || count == 0u ||
        count > KRTSP_MOSAIC_MAX) {
        return 2;
    }
    for (size_t index = 0u; index < count; ++index) {
        if (urls[index] == NULL || labels[index] == NULL) {
            return 2;
        }
    }
    (void)memset(slots, 0, sizeof(slots));

    if (!start_terminal(&session, false)) {
        return 1;
    }

    width = kittyts_width(&session);
    height = kittyts_height(&session);
    if (!rgba_size(width, height, &present_size)) {
        kittyts_stop(&session);
        g_session = NULL;
        return 1;
    }
    krtsp_attach_init(&attach);

    if (krtsp_mosaic_layout(width, height, count, 16.0f / 9.0f, tiles,
                            KRTSP_MOSAIC_MAX) != count) {
        kittyts_stop(&session);
        g_session = NULL;
        (void)fprintf(stderr,
            "kilix-rtsp: the terminal is too small for %zu cameras\n", count);
        return 1;
    }
    for (size_t index = 0u; index < count; ++index) {
        slots[index].tile = tiles[index];
        slots[index].label = labels[index];
    }
    present_buffer = malloc(present_size);
    if (present_buffer == NULL ||
        !krtsp_compositor_init(&compositor, width, height) ||
        !mosaic_start_sources(slots, count, urls, fps_cap)) {
        mosaic_stop_sources(slots, count);
        krtsp_compositor_free(&compositor);
        free(present_buffer);
        kittyts_stop(&session);
        g_session = NULL;
        return 1;
    }

    while (!g_quit) {
        int new_width;
        int new_height;
        bool any_new = false;

        /* Detached panes decode nothing; see krtsp_attach.c. */
        if (monotonic_ms() - attach_checked_at > 1000) {
            bool attached = krtsp_attach_is_attached(&attach);

            attach_checked_at = monotonic_ms();
            if (attached) {
                detached_since = 0;
            } else if (detached_since == 0) {
                detached_since = attach_checked_at;
            } else if (detached_limit > 0 &&
                       attach_checked_at - detached_since >= detached_limit) {
                exit_code = 0;
                break;
            }
            if (!attached && streaming) {
                mosaic_stop_sources(slots, count);
                streaming = false;
            } else if (attached && !streaming) {
                if (!mosaic_start_sources(slots, count, urls, fps_cap)) {
                    exit_code = 1;
                    break;
                }
                streaming = true;
                /* Restarted sources restart their frame counters, so a
                 * remembered sequence could match a frame it never saw.
                 * Forget the canvas and rebuild. */
                krtsp_compositor_reset(&compositor);
            }
        }
        if (!streaming) {
            sleep_ms(250);
            continue;
        }

        if (kittyts_check_resize(&session, &new_width, &new_height)) {
            resize_pending_at = monotonic_ms();
        }
        if (resize_pending_at != 0 &&
            monotonic_ms() - resize_pending_at > 250) {
            resize_pending_at = 0;
            new_width = kittyts_width(&session);
            new_height = kittyts_height(&session);
            if (new_width != width || new_height != height) {
                size_t new_size;
                bool layout_ok =
                    rgba_size(new_width, new_height, &new_size) &&
                    krtsp_mosaic_layout(new_width, new_height, count,
                                        16.0f / 9.0f, tiles,
                                        KRTSP_MOSAIC_MAX) == count;
                uint8_t *grown =
                    layout_ok ? realloc(present_buffer, new_size) : NULL;

                if (grown != NULL) {
                    present_buffer = grown;
                    present_size = new_size;
                    width = new_width;
                    height = new_height;
                    krtsp_compositor_free(&compositor);
                    if (!krtsp_compositor_init(&compositor, width, height)) {
                        exit_code = 1;
                        break;
                    }
                    mosaic_stop_sources(slots, count);
                    for (size_t index = 0u; index < count; ++index) {
                        slots[index].tile = tiles[index];
                    }
                    if (!mosaic_start_sources(slots, count, urls, fps_cap)) {
                        exit_code = 1;
                        break;
                    }
                }
            }
        }

        consume_input(&session);
        if (g_quit) {
            break;
        }

        /*
         * Redraw when some tile has a new frame, but no more often than
         * KRTSP_MOSAIC_MAX_FPS.
         *
         * The compositor redraws only the tiles that changed, but every
         * composite still packs a complete frame.  Seven cameras
         * arriving independently at 8-20 fps would otherwise trigger up
         * to seventy composites a second to produce at most twenty
         * visibly different frames; the cap turns most of that into one
         * composite carrying several tiles' updates.
         */
        {
            long long now = monotonic_ms();

            for (size_t index = 0u; index < count; ++index) {
                uint64_t frames =
                    krtsp_source_frame_count(slots[index].source);
                krtsp_status status =
                    krtsp_source_status(slots[index].source);
                int age_ms =
                    krtsp_source_frame_age_ms(slots[index].source);
                bool degraded = status != KRTSP_ONLINE || age_ms > 2000;

                if (frames != slots[index].last_frames ||
                    status != slots[index].last_status ||
                    (degraded && now - composed_at >= 1000)) {
                    any_new = true;
                }
            }
        }
        if (!any_new ||
            monotonic_ms() - composed_at < 1000 / KRTSP_MOSAIC_MAX_FPS) {
            sleep_ms(5);
            continue;
        }
        composed_at = monotonic_ms();

        {
            krtsp_compose_tile inputs[KRTSP_MOSAIC_MAX];
            char captions[KRTSP_MOSAIC_MAX][128];
            krtsp_damage damage[KRTSP_MOSAIC_MAX + 1];
            kittyfb_rect patch[KRTSP_MOSAIC_MAX + 1];
            size_t damage_count = 0u;
            bool composed;

            for (size_t index = 0u; index < count; ++index) {
                krtsp_status status =
                    krtsp_source_status(slots[index].source);
                int age_ms = 0;
                uint64_t sequence = 0u;
                const uint8_t *pixels = krtsp_source_borrow_latest(
                    slots[index].source, &sequence, &age_ms);

                /* Every tile is captioned.  In a grid, "which camera is
                 * that" is the first question, and an unlabelled tile
                 * that has frozen is indistinguishable from a quiet
                 * scene. */
                if (pixels == NULL) {
                    (void)snprintf(captions[index], sizeof(captions[index]),
                                   "%s  %s", slots[index].label,
                                   status == KRTSP_STARTING
                                       ? "connecting"
                                       : krtsp_status_name(status));
                } else if (status != KRTSP_ONLINE || age_ms > 2000) {
                    (void)snprintf(captions[index], sizeof(captions[index]),
                                   "%s  %s  %.0fs", slots[index].label,
                                   krtsp_status_name(status),
                                   (double)age_ms / 1000.0);
                } else {
                    (void)snprintf(captions[index], sizeof(captions[index]),
                                   "%s", slots[index].label);
                }
                inputs[index].tile = slots[index].tile;
                inputs[index].pixels = pixels;
                inputs[index].sequence = sequence;
                inputs[index].caption = captions[index];
                inputs[index].accent = status_accent(status);
                slots[index].last_frames = pixels != NULL ? sequence : 0u;
                slots[index].last_status = status;
            }

            /* Borrows are held across the composite: the blit reads the
             * ring slot directly, so releasing early would let a fresh
             * frame land under the copy. */
            composed = krtsp_compose(&compositor, inputs, count,
                                     present_buffer, present_size,
                                     damage,
                                     sizeof(damage) / sizeof(damage[0]),
                                     &damage_count);
            for (size_t index = 0u; index < count; ++index) {
                if (inputs[index].pixels != NULL) {
                    krtsp_source_release(slots[index].source);
                }
            }
            if (!composed) {
                exit_code = 1;
                break;
            }
            /* The composite says which tiles changed, so let the
             * presenter patch just those.  It falls back to a full
             * frame by itself whenever patching cannot help, so this
             * needs no reasoning about which is cheaper. */
            for (size_t index = 0u; index < damage_count; ++index) {
                patch[index].x0 = damage[index].x0;
                patch[index].y0 = damage[index].y0;
                patch[index].x1 = damage[index].x1;
                patch[index].y1 = damage[index].y1;
            }
            if (!kittyts_present_damage(&session, present_buffer, width,
                                        height, patch, damage_count)) {
                exit_code = 1;
                break;
            }
        }
        sleep_ms(8);
    }

    mosaic_stop_sources(slots, count);
    krtsp_compositor_free(&compositor);
    free(present_buffer);
    kittyts_stop(&session);
    g_session = NULL;
    return exit_code;
}
