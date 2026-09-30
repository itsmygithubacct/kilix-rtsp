/*
 * Is a frontend actually looking at this view?
 *
 * kilix runs terminal panes under kitty-pty-broker, which deliberately
 * separates a pane's lifetime from its graphical frontend: closing the
 * tab detaches the session rather than ending it, and it can be attached
 * again later.  That is a feature, and the session on disk is real, not
 * leaked.
 *
 * But a detached camera view has nobody to show frames to, and decoding
 * H.264 at 20-40% of a core to write into a journal nobody is reading is
 * pure waste.  So the view stops its stream while detached and starts it
 * again on reattach.
 *
 * Nothing about the terminal reveals this.  When a kilix tab closes there
 * is no SIGHUP, writes to the pty still succeed (the broker owns and
 * drains it), TIOCGWINSZ still reports a valid size, and the process is
 * not reparented.  Every ordinary signal a terminal program would use is
 * absent - which is why this asks the broker directly instead of trying
 * to infer it.
 *
 * When not running under a broker there is nothing to ask and nothing to
 * do: the view is always considered attached.
 */

#include "krtsp_attach.h"

#include "kitty_pty_broker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The broker exports KITTY_PTY_BROKER and KITTY_PTY_BROKER_SESSION into
 * the child, but not its runtime directory - and kpb_query_status() needs
 * it.  It is in the broker's own command line, and the broker is this
 * process's parent, so read it from there.
 */
static bool runtime_dir_from(
    pid_t pid, const char *want_session, char *out, size_t capacity)
{
    char path[64];
    char buffer[4096];
    FILE *handle;
    size_t used;
    size_t at = 0u;
    bool is_broker = false;
    bool id_matches = false;
    char found_dir[KRTSP_ATTACH_DIR_MAX];

    found_dir[0] = '\0';

    if (snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid) < 0) {
        return false;
    }
    handle = fopen(path, "rb");
    if (handle == NULL) {
        return false;
    }
    used = fread(buffer, 1u, sizeof(buffer) - 1u, handle);
    (void)fclose(handle);
    if (used == 0u) {
        return false;
    }
    buffer[used] = '\0';

    /* argv arrives NUL-separated. */
    while (at < used) {
        const char *argument = buffer + at;
        size_t length = strlen(argument);

        if (strstr(argument, "kitty-pty-broker") != NULL) {
            is_broker = true;
        }
        if (is_broker && strcmp(argument, "--runtime-dir") == 0 &&
            at + length + 1u < used) {
            const char *value = buffer + at + length + 1u;

            if (value[0] != '\0' && strlen(value) < sizeof(found_dir)) {
                (void)snprintf(found_dir, sizeof(found_dir), "%s", value);
            }
        }
        /*
         * The session id must be this broker's own.
         *
         * KITTY_PTY_BROKER_SESSION is inherited like any other variable,
         * so a process launched from a brokered pane - or by a terminal
         * that was itself started inside one - sees an id belonging to
         * somebody else's session.  Trusting it means asking about the
         * wrong pane: observed live, a visible mosaic stopped streaming
         * because an unrelated detached session answered "not attached".
         *
         * The broker that actually owns this process names its session on
         * its own command line, so require that to match.
         */
        if (is_broker && strcmp(argument, "--id") == 0 &&
            at + length + 1u < used) {
            const char *value = buffer + at + length + 1u;

            if (want_session != NULL && strcmp(value, want_session) == 0) {
                id_matches = true;
            }
        }
        at += length + 1u;
    }
    if (!id_matches || found_dir[0] == '\0' || strlen(found_dir) >= capacity) {
        return false;
    }
    (void)snprintf(out, capacity, "%s", found_dir);
    return true;
}

void krtsp_attach_init(krtsp_attach *watch)
{
    const char *under_broker;
    const char *session;

    if (watch == NULL) {
        return;
    }
    (void)memset(watch, 0, sizeof(*watch));
    watch->available = false;

    under_broker = getenv("KITTY_PTY_BROKER");
    session = getenv("KITTY_PTY_BROKER_SESSION");
    if (under_broker == NULL || strcmp(under_broker, "1") != 0 ||
        session == NULL || session[0] == '\0') {
        return;
    }
    if (strlen(session) >= sizeof(watch->session_id)) {
        return;
    }
    (void)snprintf(watch->session_id, sizeof(watch->session_id), "%s",
                   session);

    /* The parent is normally the broker; check a grandparent too, in case
     * something wraps the launch. */
    if (!runtime_dir_from(getppid(), watch->session_id, watch->runtime_dir,
                          sizeof(watch->runtime_dir))) {
        char path[64];
        FILE *handle;
        long grandparent = 0;

        if (snprintf(path, sizeof(path), "/proc/%ld/stat",
                     (long)getppid()) < 0) {
            return;
        }
        handle = fopen(path, "r");
        if (handle == NULL) {
            return;
        }
        /* Field 4 is ppid; the comm field can contain spaces, so skip to
         * the closing parenthesis first. */
        {
            char line[512];
            char *close_paren;

            if (fgets(line, sizeof(line), handle) == NULL) {
                (void)fclose(handle);
                return;
            }
            (void)fclose(handle);
            close_paren = strrchr(line, ')');
            if (close_paren == NULL ||
                sscanf(close_paren + 1, " %*c %ld", &grandparent) != 1) {
                return;
            }
        }
        if (grandparent <= 1 ||
            !runtime_dir_from((pid_t)grandparent, watch->session_id,
                              watch->runtime_dir,
                              sizeof(watch->runtime_dir))) {
            /* No ancestor broker owns this session id: the variable was
             * inherited.  Treat the view as always attached rather than
             * acting on someone else's pane. */
            return;
        }
    }
    watch->available = true;
}

bool krtsp_attach_is_attached(krtsp_attach *watch)
{
    kpb_status status;

    /* Not under a broker: there is no detachment to detect, so a view is
     * always considered watched. */
    if (watch == NULL || !watch->available) {
        return true;
    }
    if (kpb_query_status(watch->runtime_dir, watch->session_id, &status) !=
        KPB_OK) {
        /* A broker that cannot be reached is not evidence of detachment.
         * Keep streaming rather than blanking a live view because a status
         * query failed. */
        return true;
    }
    return status.attached != 0;
}

/*
 * How long a view may stay detached before it exits.
 *
 * A camera view that nobody is attached to is not a paused session worth
 * keeping: reopening the camera starts a fresh view in a second, while a
 * detached one lingers as a broker session that every later kilix start
 * re-attaches into a hidden "recovered:" tab - where it counts as watched
 * and decodes again.  On a camera desk that reopened its view daily, 29
 * copies of one stream piled up that way (2026-09-30).  So after a short
 * grace, which rides out a frontend restart, a detached view ends.
 * KILIX_RTSP_DETACHED_EXIT_SECONDS overrides the grace; 0 keeps the old
 * behaviour of waiting to be reattached for ever.
 */
long long krtsp_attach_detached_exit_ms(void)
{
    const char *value = getenv("KILIX_RTSP_DETACHED_EXIT_SECONDS");
    char *end = NULL;
    long seconds;

    if (value == NULL || value[0] == '\0') {
        return 30000;
    }
    seconds = strtol(value, &end, 10);
    if (end == value || *end != '\0' || seconds < 0 || seconds > 86400) {
        return 30000;
    }
    return (long long)seconds * 1000;
}
