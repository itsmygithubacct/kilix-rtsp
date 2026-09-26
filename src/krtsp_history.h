#ifndef KRTSP_HISTORY_H
#define KRTSP_HISTORY_H

#include <stdbool.h>
#include <time.h>

/*
 * Where in the buffer the viewer is looking, and what a key does to it.
 *
 * The rule the whole thing follows: the position is an instant on the wall
 * clock, not an offset into a file.  Live is "now"; a replay is "now, minus
 * however far back I went", and it advances with the clock, so leaving it
 * running keeps you the same distance behind rather than drifting back to
 * live or stalling.  Everything the view needs - which segment, how far
 * into it, how far behind - is derived from that one instant, so a jump, a
 * segment boundary and a resize all resolve the same way.
 *
 * No terminal, no ffmpeg and no disk in here: only keys, instants and
 * limits, which is what makes the awkward cases (the edge of the buffer,
 * pausing while live, stepping forward into live) testable.
 */

typedef enum krtsp_history_mode {
    KRTSP_HISTORY_LIVE = 0,
    KRTSP_HISTORY_REPLAY,
    KRTSP_HISTORY_PAUSED
} krtsp_history_mode;

typedef enum krtsp_history_action {
    KRTSP_HISTORY_NONE = 0,
    KRTSP_HISTORY_BACK_SHORT,
    KRTSP_HISTORY_BACK_LONG,
    KRTSP_HISTORY_FORWARD_SHORT,
    KRTSP_HISTORY_FORWARD_LONG,
    KRTSP_HISTORY_OLDEST,
    KRTSP_HISTORY_LIVE_NOW,
    KRTSP_HISTORY_TOGGLE_PAUSE
} krtsp_history_action;

#define KRTSP_HISTORY_SHORT_SECONDS 10
#define KRTSP_HISTORY_LONG_SECONDS 60
/* Within this of live counts as live: a stream is never exactly "now". */
#define KRTSP_HISTORY_LIVE_SLACK 2

typedef struct krtsp_history {
    krtsp_history_mode mode;
    time_t position;   /* the instant shown, as of `anchor` */
    time_t anchor;     /* the wall-clock second `position` was set */
    bool at_oldest;    /* the last move hit the far end of the buffer */
} krtsp_history;

void krtsp_history_init(krtsp_history *history);

/* The instant being shown at `now`. */
time_t krtsp_history_position(const krtsp_history *history, time_t now);

/* Seconds behind live at `now`; zero when live. */
int krtsp_history_behind(const krtsp_history *history, time_t now);

/*
 * Apply an action.  `oldest` is the start of the oldest segment, or zero
 * when the buffer holds nothing.  Returns true when the shown instant
 * moved or the mode changed, which is when the caller must reopen its
 * playback; a key that does nothing (back, with no buffer) returns false.
 */
bool krtsp_history_apply(
    krtsp_history *history, krtsp_history_action action, time_t now,
    time_t oldest);

/* Leave replay for live when the buffer has been trimmed out from under
 * it: the segment being watched is gone, and pretending otherwise
 * freezes the picture.  Returns true when it moved the viewer. */
bool krtsp_history_clamp(
    krtsp_history *history, time_t now, time_t oldest);

/* "-0:32", "-12:05", "-1:02:03": a status line's distance behind live. */
void krtsp_history_format(int seconds, char *out, unsigned capacity);

#endif
