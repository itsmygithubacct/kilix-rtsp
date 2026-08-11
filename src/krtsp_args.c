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
    /* A local file behaves like a camera unless told otherwise. */
    request->realtime = true;
    request->letterbox = false;
    request->pixfmt = KRTSP_PIXFMT_RGBA;
    request->legacy_timeout_flag = false;
    request->roles = (unsigned)KRTSP_ROLE_DECODE;
    request->record_dir = NULL;
    request->record_pattern = NULL;
    request->segment_seconds = 0;
    request->record_audio = false;
    request->segment_mkdir = false;
    request->segment_list = NULL;
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
    char scratch[KRTSP_ARGV_STORAGE_MAX];
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
    bool network_input;
    unsigned roles;

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
    roles = request->roles != 0u ? request->roles
                                 : (unsigned)KRTSP_ROLE_DECODE;
    {
        if ((roles & ~(unsigned)(KRTSP_ROLE_DECODE | KRTSP_ROLE_RECORD)) != 0u) {
            return 0u;
        }
        if ((roles & (unsigned)KRTSP_ROLE_DECODE) != 0u &&
            request->pixfmt != KRTSP_PIXFMT_RGBA &&
            request->pixfmt != KRTSP_PIXFMT_BGRA) {
            return 0u;
        }
        /* A recording sink with nowhere to write is a request that would
         * spawn a process guaranteed to fail; refusing here reports it as
         * the configuration error it is. */
        if ((roles & (unsigned)KRTSP_ROLE_RECORD) != 0u &&
            (request->record_dir == NULL || request->record_dir[0] == '\0')) {
            return 0u;
        }
        if (request->segment_seconds < 0) {
            return 0u;
        }
    }

    writer.argv = argv;
    writer.argv_capacity = argv_capacity;
    writer.argv_count = 0u;
    writer.storage = storage;
    writer.storage_capacity = storage_capacity;
    writer.storage_used = 0u;
    writer.failed = false;

    /* An RTSP url is the common case and the only one that takes RTSP
     * options.  rtsps:// too, and http(s):// carries a user agent but no
     * transport or socket timeout - everything else is local. */
    {
        const char *url = request->url != NULL ? request->url : "";

        network_input = strncmp(url, "rtsp://", 7) == 0 ||
                        strncmp(url, "rtsps://", 8) == 0;
    }
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
    if (network_input) {
        /*
         * RTSP options go to RTSP inputs only.
         *
         * ffmpeg refuses a demuxer option the demuxer does not have -
         * "Option rtsp_transport not found", exit 1 - so passing these
         * unconditionally means a local file or a capture device can
         * never be a source.  That is not a hypothetical: a recording
         * standing in for a camera is how the pipelines above this get
         * tested without hardware, and how a person auditions a model
         * against footage they already have.
         */
        push_arg(&writer, "-user_agent");
        push_format(&writer, "kilix-rtsp/%d.%d.%d",
                    KILIX_RTSP_VERSION_MAJOR,
                    KILIX_RTSP_VERSION_MINOR,
                    KILIX_RTSP_VERSION_PATCH);
        push_arg(&writer, "-rtsp_transport");
        push_arg(&writer, "tcp");
        push_arg(&writer,
                 request->legacy_timeout_flag ? "-stimeout" : "-timeout");
        push_arg(&writer, "10000000");
    } else if (request->realtime) {
        /*
         * A file read as fast as the disk allows is not a camera: it
         * fills the ring in a second and every consumer sees the end of
         * the recording.  -re paces it at its own frame rate, which is
         * what makes "point this at a recording" behave like pointing it
         * at the thing that recorded it.
         */
        push_arg(&writer, "-re");
    }
    push_arg(&writer, "-avoid_negative_ts");
    push_arg(&writer, "make_zero");
    push_arg(&writer, "-fflags");
    push_arg(&writer, "+genpts+discardcorrupt");
    /* Low-latency input flags trade buffering for promptness, which is
     * right for a view and wrong for an archive: a record-only process
     * has no latency to save and integrity to lose.  Honour the request
     * only when something is actually being displayed. */
    if (request->low_latency &&
        (roles & (unsigned)KRTSP_ROLE_DECODE) != 0u) {
        push_arg(&writer, "-fflags");
        push_arg(&writer, "nobuffer");
        push_arg(&writer, "-flags");
        push_arg(&writer, "low_delay");
    }
    push_arg(&writer, "-i");
    {
        /* Escape the password rather than trusting the caller to have
         * done it.  An unescaped '@' or '/' in a password does not fail
         * loudly: ffmpeg parses a different host out of the URL and
         * reports a connection error naming somewhere that does not
         * exist, which reads as a network fault rather than a quoting
         * one. */
        char escaped[KRTSP_ARGV_STORAGE_MAX];

        if (!krtsp_url_escape_password(request->url, escaped,
                                       sizeof(escaped))) {
            return 0u;
        }
        push_arg(&writer, escaped);
    }

    /*
     * The decode sink.  Audio is dropped here: this output exists to feed
     * pixels to a frame ring, and a camera's audio track would have to be
     * discarded downstream anyway.  The record sink below keeps it when
     * asked.
     */
    if ((roles & (unsigned)KRTSP_ROLE_DECODE) != 0u) {
    push_arg(&writer, "-an");
    push_arg(&writer, "-f");
    push_arg(&writer, "rawvideo");
    push_arg(&writer, "-pix_fmt");
    push_arg(&writer,
             request->pixfmt == KRTSP_PIXFMT_BGRA ? "bgra" : "rgba");

    scaling = request->width > 0;
    if (scaling || request->fps_cap > 0) {
        char filters[192];
        int printed;

        if (scaling && request->letterbox) {
            /* Fit inside the box, then pad back out to it.  The output is
             * exactly width x height whatever the camera's aspect is. */
            printed = snprintf(filters, sizeof(filters),
                "scale=%d:%d:force_original_aspect_ratio=decrease,"
                "pad=%d:%d:(ow-iw)/2:(oh-ih)/2",
                request->width, request->height,
                request->width, request->height);
        } else if (scaling) {
            printed = snprintf(filters, sizeof(filters), "scale=%d:%d",
                               request->width, request->height);
        } else {
            printed = 0;
            filters[0] = '\0';
        }
        if (printed < 0 || (size_t)printed >= sizeof(filters)) {
            return 0u;
        }
        if (request->fps_cap > 0) {
            char with_fps[224];

            printed = snprintf(with_fps, sizeof(with_fps), "%s%sfps=%d",
                               filters, filters[0] != '\0' ? "," : "",
                               request->fps_cap);
            if (printed < 0 || (size_t)printed >= sizeof(with_fps)) {
                return 0u;
            }
            push_arg(&writer, "-vf");
            push_arg(&writer, with_fps);
        } else {
            push_arg(&writer, "-vf");
            push_arg(&writer, filters);
        }
    }

    /* Frames go to stdout, where a fixed pixel format makes the pipe
     * self-framing. */
    push_arg(&writer, "-");
    }

    /*
     * The record sink.  One process, one RTSP session, a second output:
     * this is what lets a camera feed detection and archive itself
     * without opening a connection twice.
     *
     * -c copy is not an optimisation, it is the whole point.  The camera
     * already encoded these bytes; re-encoding them at the same
     * resolution measurably produces *larger* files while burning a core
     * to do it.  Copying costs neither.
     */
    if ((roles & (unsigned)KRTSP_ROLE_RECORD) != 0u) {
        const char *pattern = request->record_pattern != NULL &&
                                      request->record_pattern[0] != '\0'
                                  ? request->record_pattern
                                  : "%Y-%m-%d_%H.%M.%S.mkv";
        int seconds = request->segment_seconds > 0
                          ? request->segment_seconds
                          : 10;

        push_arg(&writer, "-map");
        push_arg(&writer, "0:v");
        if (request->record_audio) {
            /* '?' keeps a camera with no audio track from failing the
             * whole process. */
            push_arg(&writer, "-map");
            push_arg(&writer, "0:a?");
        }
        push_arg(&writer, "-c");
        push_arg(&writer, "copy");
        push_arg(&writer, "-f");
        push_arg(&writer, "segment");
        push_arg(&writer, "-segment_time");
        push_format(&writer, "%d", seconds);
        /* Each segment starts at zero, so a file is playable alone rather
         * than only as part of the sequence that preceded it. */
        push_arg(&writer, "-reset_timestamps");
        push_arg(&writer, "1");
        push_arg(&writer, "-strftime");
        push_arg(&writer, "1");
        if (request->segment_list != NULL &&
            request->segment_list[0] != '\0') {
            /* Bounded and rewritten in place: a supervisor stats this one
             * file to know recording is alive, and it must not grow
             * without limit over a month of uptime. */
            push_arg(&writer, "-segment_list");
            push_arg(&writer, request->segment_list);
            push_arg(&writer, "-segment_list_size");
            push_arg(&writer, "8");
            push_arg(&writer, "-segment_list_flags");
            push_arg(&writer, "+live");
        }
        /* Only when the binary has it.  A hierarchical pattern needs the
         * muxer to create directories, and passing the flag to an ffmpeg
         * without it is an unrecognised-option failure at spawn. */
        if (request->segment_mkdir) {
            push_arg(&writer, "-strftime_mkdir");
            push_arg(&writer, "1");
        }
        push_format(&writer, "%s/%s", request->record_dir, pattern);
    }

    if (writer.failed) {
        return 0u;
    }
    argv[writer.argv_count] = NULL;
    return writer.argv_count;
}

/*
 * Locate the userinfo in an absolute URL.
 *
 * Returns false when there is none.  On success `colon` points at the
 * separator between user and password, or NULL when the userinfo carries
 * no password, and `at` points at the '@' that ends it.
 *
 * The '@' taken is the LAST one inside the authority, not the first: '@'
 * is legal in a password, and taking the first one both truncates the
 * password and leaks its tail into what is then parsed as a hostname.
 */
static bool find_userinfo(
    const char *url, const char **colon, const char **at)
{
    const char *scheme_end = strstr(url, "://");
    const char *authority;
    const char *authority_end;
    const char *last_at = NULL;

    *colon = NULL;
    *at = NULL;
    if (scheme_end == NULL) {
        return false;
    }
    authority = scheme_end + 3;
    /* The authority ends at the first '/', '?' or '#'; anything after
     * that is path and cannot hold userinfo. */
    authority_end = authority + strcspn(authority, "/?#");
    for (const char *scan = authority; scan < authority_end; ++scan) {
        if (*scan == '@') {
            last_at = scan;
        }
    }
    if (last_at == NULL) {
        return false;
    }
    *at = last_at;
    *colon = memchr(authority, ':', (size_t)(last_at - authority));
    return true;
}

bool krtsp_url_escape_password(const char *url, char *out, size_t capacity)
{
    /* RFC 3986 unreserved set.  Everything else in the password is
     * encoded, which is always safe: a percent-encoded unreserved
     * character would also be understood, but leaving them alone keeps
     * ordinary passwords readable in a process listing. */
    static const char unreserved[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    static const char hex[] = "0123456789ABCDEF";
    const char *colon;
    const char *at;
    size_t used = 0u;

    if (out == NULL || capacity == 0u) {
        return false;
    }
    out[0] = '\0';
    if (url == NULL) {
        return false;
    }
    if (!find_userinfo(url, &colon, &at) || colon == NULL) {
        /* No password to escape. */
        size_t length = strlen(url);

        if (length + 1u > capacity) {
            return false;
        }
        memcpy(out, url, length + 1u);
        return true;
    }

    /* Everything up to and including the ':' is copied unchanged. */
    {
        size_t prefix = (size_t)(colon + 1 - url);

        if (prefix + 1u > capacity) {
            return false;
        }
        memcpy(out, url, prefix);
        used = prefix;
    }

    for (const char *scan = colon + 1; scan < at; ++scan) {
        unsigned char character = (unsigned char)*scan;

        if (character == '%' && (size_t)(at - scan) >= 3u &&
            ((scan[1] >= '0' && scan[1] <= '9') ||
             (scan[1] >= 'a' && scan[1] <= 'f') ||
             (scan[1] >= 'A' && scan[1] <= 'F')) &&
            ((scan[2] >= '0' && scan[2] <= '9') ||
             (scan[2] >= 'a' && scan[2] <= 'f') ||
             (scan[2] >= 'A' && scan[2] <= 'F'))) {
            /* The config format requires '/', '?' and '#' in a password to
             * arrive percent-encoded because a URL parser cannot recover
             * their raw form.  Preserve a valid triplet rather than encoding
             * its '%' again and silently changing the password. */
            if (used + 4u > capacity) {
                out[0] = '\0';
                return false;
            }
            out[used++] = *scan++;
            out[used++] = *scan++;
            out[used++] = *scan;
        } else if (strchr(unreserved, character) != NULL && character != '\0') {
            if (used + 2u > capacity) {
                out[0] = '\0';
                return false;
            }
            out[used++] = (char)character;
        } else {
            /* Note: %20 for space rather than '+'.  A '+' in userinfo is
             * a literal plus, not a space, so form encoding would be
             * wrong here even though it is common elsewhere. */
            if (used + 4u > capacity) {
                out[0] = '\0';
                return false;
            }
            out[used++] = '%';
            out[used++] = hex[(character >> 4) & 0x0Fu];
            out[used++] = hex[character & 0x0Fu];
        }
    }

    {
        size_t tail = strlen(at);

        if (used + tail + 1u > capacity) {
            out[0] = '\0';
            return false;
        }
        memcpy(out + used, at, tail + 1u);
    }
    return true;
}

bool krtsp_url_redact(const char *url, char *out, size_t capacity)
{
    static const char mask[] = "***";
    const char *colon;
    const char *at;
    size_t prefix;
    size_t needed;

    if (out == NULL || capacity == 0u) {
        return false;
    }
    out[0] = '\0';
    if (url == NULL) {
        return false;
    }
    if (!find_userinfo(url, &colon, &at)) {
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
    prefix = colon != NULL
        ? (size_t)(colon + 1 - url)
        : (size_t)(at - url);

    needed = prefix + (colon != NULL ? sizeof(mask) - 1u : 0u) +
             strlen(at) + 1u;
    if (needed > capacity) {
        return false;
    }
    memcpy(out, url, prefix);
    if (colon != NULL) {
        memcpy(out + prefix, mask, sizeof(mask) - 1u);
        prefix += sizeof(mask) - 1u;
    }
    memcpy(out + prefix, at, strlen(at) + 1u);
    return true;
}
