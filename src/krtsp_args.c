/*
 * ffmpeg argument construction.
 *
 * Kept free of I/O so the flag policy - which is where an RTSP client is
 * actually right or wrong - is testable without spawning anything.
 *
 * The RTSP flags come from Frigate, which has run them against a large
 * and varied fleet of cameras for years.  Each earns its place:
 *
 *   -rtsp_transport tcp      UDP loses packets on wifi cameras; TCP trades
 *                            latency for intact frames
 *   -fflags +discardcorrupt  drop damaged frames rather than render artifacts
 *   -fflags +genpts          many cameras emit broken or absent timestamps
 *   -avoid_negative_ts       negative timestamps break downstream muxers
 *   -timeout 10000000        10s socket timeout.  This is the defence against
 *                            the characteristic camera failure: wedging while
 *                            holding the TCP connection open, so the socket
 *                            stays valid and a read blocks forever
 *   -threads 2               fewer caused segment problems in Frigate (#5659)
 *
 * Two deliberate departures from Frigate's own defaults:
 *
 *   - rgba/bgra rather than yuv420p, because this frame is going to a
 *     terminal presenter and not to an object detector.
 *   - nobuffer/low_delay available by default, because Frigate's generic
 *     preset is tuned for recording integrity and this is tuned for
 *     latency.  Frigate's own low-latency restream preset is the closer
 *     precedent.
 *
 * -use_wallclock_as_timestamps is deliberately absent: it exists for
 * recording correctness, and nothing here muxes.  Frames are read in
 * order and the newest is presented.
 */

#include "kilix_rtsp.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void krtsp_args_request_init(krtsp_args_request *request)
{
    if (request == NULL) {
        return;
    }
    request->url = NULL;
    request->width = 0;
    request->height = 0;
    request->fps_cap = 0;
    request->low_latency = true;
    request->pixfmt = KRTSP_PIXFMT_RGBA;
    request->legacy_timeout_flag = false;
}

/* Append one NUL-terminated argument, pointing the next argv slot at it. */
typedef struct arg_writer {
    char **argv;
    size_t argv_capacity;
    size_t argv_count;
    char *storage;
    size_t storage_capacity;
    size_t storage_used;
    bool failed;
} arg_writer;

static void push_arg(arg_writer *w, const char *text)
{
    size_t length;

    if (w->failed) {
        return;
    }
    /* One slot is reserved for the NULL terminator. */
    if (w->argv_count + 1u >= w->argv_capacity) {
        w->failed = true;
        return;
    }
    length = strlen(text) + 1u;
    if (length > w->storage_capacity - w->storage_used) {
        w->failed = true;
        return;
    }
    memcpy(w->storage + w->storage_used, text, length);
    w->argv[w->argv_count++] = w->storage + w->storage_used;
    w->storage_used += length;
}

static void push_format(arg_writer *w, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void push_format(arg_writer *w, const char *format, ...)
{
    char scratch[256];
    va_list arguments;
    int printed;

    if (w->failed) {
        return;
    }
    va_start(arguments, format);
    printed = vsnprintf(scratch, sizeof(scratch), format, arguments);
    va_end(arguments);
    if (printed < 0 || (size_t)printed >= sizeof(scratch)) {
        w->failed = true;
        return;
    }
    push_arg(w, scratch);
}

size_t krtsp_build_argv(
    const krtsp_args_request *request,
    char **argv,
    size_t argv_capacity,
    char *storage,
    size_t storage_capacity)
{
    arg_writer writer;
    bool scaling;

    if (request == NULL || argv == NULL || storage == NULL ||
        argv_capacity == 0u || storage_capacity == 0u) {
        return 0u;
    }
    if (request->url == NULL || request->url[0] == '\0') {
        return 0u;
    }
    /* Either both dimensions or neither: a half-specified scale silently
     * changes aspect ratio, and the caller sizes frame buffers from these. */
    if ((request->width > 0) != (request->height > 0)) {
        return 0u;
    }
    if (request->width < 0 || request->height < 0 || request->fps_cap < 0) {
        return 0u;
    }

    writer.argv = argv;
    writer.argv_capacity = argv_capacity;
    writer.argv_count = 0u;
    writer.storage = storage;
    writer.storage_capacity = storage_capacity;
    writer.storage_used = 0u;
    writer.failed = false;

    push_arg(&writer, "ffmpeg");

    /* Global. */
    push_arg(&writer, "-hide_banner");
    push_arg(&writer, "-loglevel");
    push_arg(&writer, "warning");
    push_arg(&writer, "-threads");
    push_arg(&writer, "2");
    /* Without this ffmpeg competes with the viewer for terminal input. */
    push_arg(&writer, "-nostdin");

    /* Input. */
    push_arg(&writer, "-user_agent");
    push_format(&writer, "kilix-rtsp/%d.%d.%d",
                KILIX_RTSP_VERSION_MAJOR,
                KILIX_RTSP_VERSION_MINOR,
                KILIX_RTSP_VERSION_PATCH);
    push_arg(&writer, "-rtsp_transport");
    push_arg(&writer, "tcp");
    push_arg(&writer, request->legacy_timeout_flag ? "-stimeout" : "-timeout");
    push_arg(&writer, "10000000");
    push_arg(&writer, "-avoid_negative_ts");
    push_arg(&writer, "make_zero");
    push_arg(&writer, "-fflags");
    push_arg(&writer, "+genpts+discardcorrupt");
    if (request->low_latency) {
        push_arg(&writer, "-fflags");
        push_arg(&writer, "nobuffer");
        push_arg(&writer, "-flags");
        push_arg(&writer, "low_delay");
    }
    push_arg(&writer, "-i");
    push_arg(&writer, request->url);

    /* Output.  Audio is dropped: playback is a later concern and a
     * camera's audio track otherwise has to be muxed or discarded
     * downstream anyway. */
    push_arg(&writer, "-an");
    push_arg(&writer, "-f");
    push_arg(&writer, "rawvideo");
    push_arg(&writer, "-pix_fmt");
    push_arg(&writer,
             request->pixfmt == KRTSP_PIXFMT_BGRA ? "bgra" : "rgba");

    scaling = request->width > 0;
    if (scaling || request->fps_cap > 0) {
        push_arg(&writer, "-vf");
        if (scaling && request->fps_cap > 0) {
            push_format(&writer, "scale=%d:%d,fps=%d",
                        request->width, request->height, request->fps_cap);
        } else if (scaling) {
            push_format(&writer, "scale=%d:%d",
                        request->width, request->height);
        } else {
            push_format(&writer, "fps=%d", request->fps_cap);
        }
    }

    /* Frames go to stdout, where a fixed pixel format makes the pipe
     * self-framing. */
    push_arg(&writer, "-");

    if (writer.failed) {
        return 0u;
    }
    argv[writer.argv_count] = NULL;
    return writer.argv_count;
}

bool krtsp_url_redact(const char *url, char *out, size_t capacity)
{
    static const char mask[] = "***";
    const char *scheme_end;
    const char *authority;
    const char *authority_end;
    const char *at;
    const char *colon;
    size_t prefix;
    size_t needed;

    if (out == NULL || capacity == 0u) {
        return false;
    }
    out[0] = '\0';
    if (url == NULL) {
        return false;
    }

    scheme_end = strstr(url, "://");
    at = NULL;
    if (scheme_end != NULL) {
        authority = scheme_end + 3;
        /* The authority ends at the first '/', '?' or '#'; anything past
         * that is path and cannot contain userinfo. */
        authority_end = authority + strcspn(authority, "/?#");
        /* The LAST '@' inside the authority delimits userinfo.  Using the
         * first one leaks the tail of any password containing '@', which
         * is legal there and does occur. */
        for (const char *scan = authority; scan < authority_end; ++scan) {
            if (*scan == '@') {
                at = scan;
            }
        }
    }
    if (at == NULL) {
        /* No userinfo: nothing to hide. */
        needed = strlen(url);
        if (needed + 1u > capacity) {
            return false;
        }
        memcpy(out, url, needed + 1u);
        return true;
    }

    /* Keep the username, mask the password: knowing which account a
     * camera uses is useful in a log and is not the secret. */
    colon = memchr(scheme_end + 3, ':', (size_t)(at - (scheme_end + 3)));
    prefix = colon != NULL
        ? (size_t)(colon + 1 - url)
        : (size_t)(at - url);

    needed = prefix + (colon != NULL ? sizeof(mask) - 1u : 0u) +
             strlen(at) + 1u;
    if (needed > capacity) {
        return false;
    }
    memcpy(out, url, prefix);
    out[prefix] = '\0';
    if (colon != NULL) {
        memcpy(out + prefix, mask, sizeof(mask) - 1u);
        out[prefix + sizeof(mask) - 1u] = '\0';
    }
    strcat(out, at);
    return true;
}
