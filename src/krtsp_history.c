#include "krtsp_history.h"

#include <stdio.h>
#include <string.h>

void krtsp_history_init(krtsp_history *history)
{
    if (history != NULL) {
        (void)memset(history, 0, sizeof(*history));
        history->mode = KRTSP_HISTORY_LIVE;
    }
}

time_t krtsp_history_position(const krtsp_history *history, time_t now)
{
    if (history == NULL || history->mode == KRTSP_HISTORY_LIVE) {
        return now;
    }
    if (history->mode == KRTSP_HISTORY_PAUSED || now <= history->anchor) {
        return history->position;
    }
    return history->position + (now - history->anchor);
}

int krtsp_history_behind(const krtsp_history *history, time_t now)
{
    time_t position;

    if (history == NULL || history->mode == KRTSP_HISTORY_LIVE) {
        return 0;
    }
    position = krtsp_history_position(history, now);
    return position >= now ? 0 : (int)(now - position);
}

static void go(krtsp_history *history, krtsp_history_mode mode,
               time_t position, time_t now)
{
    history->mode = mode;
    history->position = position;
    history->anchor = now;
}

bool krtsp_history_apply(
    krtsp_history *history, krtsp_history_action action, time_t now,
    time_t oldest)
{
    time_t here;
    long step = 0;
    bool forward = false;

    if (history == NULL) {
        return false;
    }
    here = krtsp_history_position(history, now);

    switch (action) {
    case KRTSP_HISTORY_BACK_SHORT:
        step = KRTSP_HISTORY_SHORT_SECONDS;
        break;
    case KRTSP_HISTORY_BACK_LONG:
        step = KRTSP_HISTORY_LONG_SECONDS;
        break;
    case KRTSP_HISTORY_FORWARD_SHORT:
        step = KRTSP_HISTORY_SHORT_SECONDS;
        forward = true;
        break;
    case KRTSP_HISTORY_FORWARD_LONG:
        step = KRTSP_HISTORY_LONG_SECONDS;
        forward = true;
        break;
    case KRTSP_HISTORY_OLDEST:
        if (oldest == 0) {
            return false;
        }
        history->at_oldest = true;
        go(history, history->mode == KRTSP_HISTORY_PAUSED
                        ? KRTSP_HISTORY_PAUSED : KRTSP_HISTORY_REPLAY,
           oldest, now);
        return true;
    case KRTSP_HISTORY_LIVE_NOW:
        if (history->mode == KRTSP_HISTORY_LIVE) {
            return false;
        }
        history->at_oldest = false;
        go(history, KRTSP_HISTORY_LIVE, now, now);
        return true;
    case KRTSP_HISTORY_TOGGLE_PAUSE:
        if (history->mode == KRTSP_HISTORY_PAUSED) {
            go(history, KRTSP_HISTORY_REPLAY, here, now);
        } else {
            /* Pausing live freezes "now"; the distance behind then grows
             * by a second a second, which is the truth of it. */
            go(history, KRTSP_HISTORY_PAUSED, here, now);
        }
        return true;
    case KRTSP_HISTORY_NONE:
    default:
        return false;
    }

    if (forward) {
        time_t target;

        if (history->mode == KRTSP_HISTORY_LIVE) {
            return false;   /* nothing ahead of now */
        }
        target = here + step;
        history->at_oldest = false;
        if (target >= now - KRTSP_HISTORY_LIVE_SLACK) {
            go(history, KRTSP_HISTORY_LIVE, now, now);
        } else {
            go(history, history->mode, target, now);
        }
        return true;
    }

    /* Back. */
    if (oldest == 0) {
        return false;   /* no history yet: nothing to go back into */
    }
    {
        time_t target = here - step;
        krtsp_history_mode mode = history->mode == KRTSP_HISTORY_PAUSED
                                      ? KRTSP_HISTORY_PAUSED
                                      : KRTSP_HISTORY_REPLAY;

        if (target <= oldest) {
            if (history->mode != KRTSP_HISTORY_LIVE && here <= oldest) {
                history->at_oldest = true;
                return false;   /* already at the far end */
            }
            target = oldest;
            history->at_oldest = true;
        } else {
            history->at_oldest = false;
        }
        go(history, mode, target, now);
    }
    return true;
}

bool krtsp_history_clamp(
    krtsp_history *history, time_t now, time_t oldest)
{
    if (history == NULL || history->mode == KRTSP_HISTORY_LIVE) {
        return false;
    }
    if (oldest == 0 || krtsp_history_position(history, now) < oldest) {
        /* Retention has caught up with the viewer; the moment they were
         * watching no longer exists.  Show the oldest that does, or live
         * when nothing is left. */
        if (oldest == 0) {
            go(history, KRTSP_HISTORY_LIVE, now, now);
        } else {
            go(history, history->mode, oldest, now);
            history->at_oldest = true;
        }
        return true;
    }
    return false;
}

void krtsp_history_format(int seconds, char *out, unsigned capacity)
{
    int hours;
    int minutes;
    int rest;

    if (out == NULL || capacity == 0u) {
        return;
    }
    if (seconds < 0) {
        seconds = 0;
    }
    hours = seconds / 3600;
    minutes = (seconds % 3600) / 60;
    rest = seconds % 60;
    if (hours > 0) {
        (void)snprintf(out, capacity, "-%d:%02d:%02d", hours, minutes, rest);
    } else {
        (void)snprintf(out, capacity, "-%d:%02d", minutes, rest);
    }
}
