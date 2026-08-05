/*
 * The view command: one camera filling the terminal.
 *
 * Frames arrive from ffmpeg already scaled and letterboxed to exactly the
 * framebuffer size, so the loop never scales anything - it borrows the
 * newest frame, optionally draws a status banner over it, and presents.
 *
 * Two decisions worth stating:
 *
 * Frames are requested as BGRA.  soft-raster's canvas is uint32
 * 0xAARRGGBB, which on a little-endian machine is the byte order B,G,R,A,
 * so a BGRA frame can be wrapped as a canvas with no conversion.
 * sr_pack_rgba() then produces the R,G,B,A order kittyts_present() wants.
 * Asking for RGBA instead and wrapping it would swap red and blue.
 *
 * The loop presents only when the frame sequence changes.  These cameras
 * deliver 8-20 fps; presenting the same frame at 60 would spend the whole
 * transport budget re-sending pixels that did not change.
 */

#include "kilix_rtsp.h"
#include "krtsp_view.h"
#include "krtsp_attach.h"

#include "kitty_terminal_session.h"
#include "soft_raster.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/*
 * Draw a status banner.
 *
 * This is the most valuable thing on the screen.  A camera that froze an
 * hour ago keeps handing back a perfectly valid frame forever, and it is
 * indistinguishable from a quiet driveway until something says otherwise.
 */
static void draw_banner(sr_canvas *canvas, const char *text, uint32_t accent)
{
    const int scale = canvas->w >= 960 ? 2 : 1;
    int text_width = sr_text_width(text, scale);
    int pad = 6 * scale;
    int height = SR_FONT_H * scale + pad * 2;
    int width = text_width + pad * 2;

    if (width > canvas->w) {
        width = canvas->w;
    }
    /* A translucent plate keeps the text readable over a bright scene
     * without hiding the part of the picture that matters. */
    for (int y = 0; y < height && y < canvas->h; ++y) {
        for (int x = 0; x < width; ++x) {
            sr_blend(canvas, x, y, 0x000000u, 0.55f);
        }
    }
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
static bool start_terminal(kittyts_session *session)
{
    kittyts_options options;

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

int krtsp_view_run(const char *url, const char *label, int fps_cap)
{
    kittyts_session session;
    krtsp_source *source = NULL;
    krtsp_source_options source_options;
    sr_canvas canvas;
    krtsp_attach attach;
    bool streaming = true;
    long long attach_checked_at = 0;
    uint8_t *present_buffer = NULL;
    uint64_t last_sequence = UINT64_MAX;
    long long resize_pending_at = 0;
    int width;
    int height;
    int exit_code = 0;

    if (url == NULL || label == NULL) {
        return 2;
    }

    if (!start_terminal(&session)) {
        return 1;
    }

    width = kittyts_width(&session);
    height = kittyts_height(&session);
    krtsp_attach_init(&attach);

    krtsp_source_options_init(&source_options);
    source_options.width = width;
    source_options.height = height;
    source_options.fps_cap = fps_cap;
    /* ffmpeg letterboxes, so every frame is exactly framebuffer-sized and
     * the loop never scales. */
    source_options.letterbox = true;
    source_options.pixfmt = KRTSP_PIXFMT_BGRA;

    if (!krtsp_source_start(&source, url, &source_options)) {
        kittyts_stop(&session);
        g_session = NULL;
        (void)fprintf(stderr, "kilix-rtsp: cannot start the source\n");
        return 1;
    }

    present_buffer = malloc((size_t)width * (size_t)height * 4u);
    if (present_buffer == NULL) {
        krtsp_source_stop(source);
        kittyts_stop(&session);
        g_session = NULL;
        return 1;
    }

    while (!g_quit) {
        int new_width;
        int new_height;
        int age_ms = 0;
        const uint8_t *pixels;
        krtsp_status status;
        char banner[192];

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
        if (monotonic_ms() - attach_checked_at > 1000) {
            bool attached = krtsp_attach_is_attached(&attach);

            attach_checked_at = monotonic_ms();
            if (!attached && streaming) {
                krtsp_source_stop(source);
                source = NULL;
                streaming = false;
            } else if (attached && !streaming) {
                if (!krtsp_source_start(&source, url, &source_options)) {
                    exit_code = 1;
                    break;
                }
                streaming = true;
                last_sequence = UINT64_MAX;
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
            resize_pending_at = monotonic_ms();
        }
        if (resize_pending_at != 0 &&
            monotonic_ms() - resize_pending_at > 250) {
            resize_pending_at = 0;
            new_width = kittyts_width(&session);
            new_height = kittyts_height(&session);
            if (new_width != width || new_height != height) {
                uint8_t *grown =
                    realloc(present_buffer,
                            (size_t)new_width * (size_t)new_height * 4u);

                if (grown != NULL) {
                    present_buffer = grown;
                    width = new_width;
                    height = new_height;
                    krtsp_source_stop(source);
                    source = NULL;
                    source_options.width = width;
                    source_options.height = height;
                    if (!krtsp_source_start(&source, url, &source_options)) {
                        exit_code = 1;
                        break;
                    }
                    last_sequence = UINT64_MAX;
                }
            }
        }

        consume_input(&session);
        if (g_quit) {
            break;
        }

        status = krtsp_source_status(source);
        pixels = krtsp_source_borrow(source, &age_ms);

        if (pixels == NULL) {
            /* Nothing yet.  Say what is happening rather than showing a
             * black screen: a camera can take several seconds to produce
             * its first frame, and silence looks like a failure. */
            sr_canvas_wrap(&canvas, (uint32_t *)(void *)present_buffer,
                           width, height);
            (void)snprintf(banner, sizeof(banner), "%s: %s", label,
                           status == KRTSP_STARTING ? "connecting"
                                                    : krtsp_status_name(status));
            draw_centered_notice(&canvas, banner);
            (void)sr_pack_rgba(&canvas, present_buffer,
                               (size_t)width * (size_t)height * 4u);
            (void)kittyts_present(&session, present_buffer, width, height);
            sleep_ms(200);
            continue;
        }

        {
            krtsp_source_stats stats;
            bool degraded = status != KRTSP_ONLINE || age_ms > 2000;
            bool changed;

            /*
             * Present only when a new frame has actually arrived.  The
             * frame counter is the signal; re-sending an unchanged frame
             * spends the whole transport budget for no visible gain, and
             * these cameras deliver 8-20 fps against a loop that could
             * spin far faster.
             *
             * A degraded source is the exception: the banner carries a
             * live age, so it has to be redrawn even when the picture
             * behind it is frozen.
             */
            krtsp_source_get_stats(source, &stats);
            changed = stats.frames != last_sequence;
            if (!changed && !degraded) {
                krtsp_source_release(source);
                sleep_ms(8);
                continue;
            }
            last_sequence = stats.frames;

            if (degraded) {
                /* Drawing over the frame needs a private copy: the
                 * borrowed buffer belongs to the source. */
                memcpy(present_buffer, pixels,
                       (size_t)width * (size_t)height * 4u);
                krtsp_source_release(source);
                sr_canvas_wrap(&canvas, (uint32_t *)(void *)present_buffer,
                               width, height);
                (void)snprintf(banner, sizeof(banner), "%s  %s  %.1fs old",
                               label, krtsp_status_name(status),
                               (double)age_ms / 1000.0);
                draw_banner(&canvas, banner, status_accent(status));
                (void)sr_pack_rgba(&canvas, present_buffer,
                                   (size_t)width * (size_t)height * 4u);
            } else {
                /* Nothing is drawn over a healthy frame, so pack straight
                 * out of the borrowed buffer.  sr_pack_rgba() takes a
                 * const canvas and writes elsewhere, so this skips a
                 * full-frame copy - 7.7 MB per frame at full screen - in
                 * the case that runs essentially all the time. */
                sr_canvas_wrap(&canvas, (uint32_t *)(void *)(uintptr_t)pixels,
                               width, height);
                (void)sr_pack_rgba(&canvas, present_buffer,
                                   (size_t)width * (size_t)height * 4u);
                krtsp_source_release(source);
            }
            if (!kittyts_present(&session, present_buffer, width, height)) {
                exit_code = 1;
                break;
            }
        }
        sleep_ms(8);
    }

    free(present_buffer);
    krtsp_source_stop(source);
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
    sr_canvas canvas;
    uint8_t *present_buffer = NULL;
    long long attach_checked_at = 0;
    long long resize_pending_at = 0;
    long long composed_at = 0;
    bool streaming = true;
    int width;
    int height;
    int exit_code = 0;

    if (urls == NULL || labels == NULL || count == 0u ||
        count > KRTSP_MOSAIC_MAX) {
        return 2;
    }
    (void)memset(slots, 0, sizeof(slots));

    if (!start_terminal(&session)) {
        return 1;
    }

    width = kittyts_width(&session);
    height = kittyts_height(&session);
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
    present_buffer = malloc((size_t)width * (size_t)height * 4u);
    if (present_buffer == NULL || !mosaic_start_sources(slots, count, urls,
                                                        fps_cap)) {
        mosaic_stop_sources(slots, count);
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
            if (!attached && streaming) {
                mosaic_stop_sources(slots, count);
                streaming = false;
            } else if (attached && !streaming) {
                if (!mosaic_start_sources(slots, count, urls, fps_cap)) {
                    exit_code = 1;
                    break;
                }
                streaming = true;
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
                uint8_t *grown = realloc(
                    present_buffer,
                    (size_t)new_width * (size_t)new_height * 4u);

                if (grown != NULL &&
                    krtsp_mosaic_layout(new_width, new_height, count,
                                        16.0f / 9.0f, tiles,
                                        KRTSP_MOSAIC_MAX) == count) {
                    present_buffer = grown;
                    width = new_width;
                    height = new_height;
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
         * Compositing is per-canvas, not per-tile: one new frame costs a
         * full clear, seven blits, seven captions and a pack of the whole
         * canvas.  Seven cameras arriving independently at 8-20 fps would
         * otherwise trigger up to seventy of those a second to produce at
         * most twenty visibly different frames.  The cap turns most of
         * that into one composite carrying several tiles' updates.
         */
        for (size_t index = 0u; index < count; ++index) {
            krtsp_source_stats stats;

            krtsp_source_get_stats(slots[index].source, &stats);
            if (stats.frames != slots[index].last_frames) {
                any_new = true;
            }
        }
        if (!any_new ||
            monotonic_ms() - composed_at < 1000 / KRTSP_MOSAIC_MAX_FPS) {
            sleep_ms(5);
            continue;
        }
        composed_at = monotonic_ms();

        sr_canvas_wrap(&canvas, (uint32_t *)(void *)present_buffer,
                       width, height);
        sr_clear(&canvas, 0x0A0A0Cu);

        for (size_t index = 0u; index < count; ++index) {
            const krtsp_tile *tile = &slots[index].tile;
            krtsp_source_stats stats;
            krtsp_status status = krtsp_source_status(slots[index].source);
            const uint8_t *pixels;
            int age_ms = 0;
            char caption[128];

            krtsp_source_get_stats(slots[index].source, &stats);
            slots[index].last_frames = stats.frames;

            pixels = krtsp_source_borrow(slots[index].source, &age_ms);
            if (pixels != NULL) {
                sr_canvas source_canvas;

                sr_canvas_wrap(&source_canvas,
                               (uint32_t *)(void *)(uintptr_t)pixels,
                               tile->width, tile->height);
                sr_blit(&canvas, &source_canvas, tile->x, tile->y);
                krtsp_source_release(slots[index].source);
            } else {
                sr_fill_rect(&canvas, (float)tile->x, (float)tile->y,
                             (float)tile->width, (float)tile->height,
                             0x141418u, 1.0f);
            }

            /* Every tile is captioned.  In a grid, "which camera is
             * that" is the first question, and an unlabelled tile that
             * has frozen is indistinguishable from a quiet scene. */
            if (pixels == NULL) {
                (void)snprintf(caption, sizeof(caption), "%s  %s",
                               slots[index].label,
                               status == KRTSP_STARTING ? "connecting"
                                                        : krtsp_status_name(status));
            } else if (status != KRTSP_ONLINE || age_ms > 2000) {
                (void)snprintf(caption, sizeof(caption), "%s  %s  %.0fs",
                               slots[index].label, krtsp_status_name(status),
                               (double)age_ms / 1000.0);
            } else {
                (void)snprintf(caption, sizeof(caption), "%s",
                               slots[index].label);
            }
            {
                int pad = 4;
                int band = SR_FONT_H + pad;

                for (int y = 0; y < band && tile->y + y < height; ++y) {
                    for (int x = 0; x < tile->width; ++x) {
                        sr_blend(&canvas, tile->x + x, tile->y + y,
                                 0x000000u, 0.5f);
                    }
                }
                sr_fill_rect(&canvas, (float)tile->x, (float)tile->y, 3.0f,
                             (float)band,
                             status_accent(status), 1.0f);
                sr_text_shadow(&canvas, (float)(tile->x + pad + 3),
                               (float)(tile->y + pad / 2), caption,
                               0xFFFFFFu, 1.0f, 1);
            }
        }

        (void)sr_pack_rgba(&canvas, present_buffer,
                           (size_t)width * (size_t)height * 4u);
        if (!kittyts_present(&session, present_buffer, width, height)) {
            exit_code = 1;
            break;
        }
        sleep_ms(8);
    }

    mosaic_stop_sources(slots, count);
    free(present_buffer);
    kittyts_stop(&session);
    g_session = NULL;
    return exit_code;
}
