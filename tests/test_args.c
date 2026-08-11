#include "kilix_rtsp.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

static char storage[KRTSP_ARGV_STORAGE_MAX];
static char *argv[KRTSP_ARGV_MAX];

/* A URL shaped like the ones on a real install: credentials in the
 * userinfo, which is what makes a camera URL a secret. */
static const char sample_url[] = "rtsp://operator:hunter2@203.0.113.9:554/ch1";

static bool has_arg(size_t count, const char *needle)
{
    for (size_t index = 0u; index < count; ++index) {
        if (strcmp(argv[index], needle) == 0) {
            return true;
        }
    }
    return false;
}

/* True when `first` appears immediately followed by `second`. */
static bool has_pair(size_t count, const char *first, const char *second)
{
    for (size_t index = 0u; index + 1u < count; ++index) {
        if (strcmp(argv[index], first) == 0 &&
            strcmp(argv[index + 1], second) == 0) {
            return true;
        }
    }
    return false;
}

static size_t count_arg(size_t count, const char *needle)
{
    size_t found = 0u;

    for (size_t index = 0u; index < count; ++index) {
        if (strcmp(argv[index], needle) == 0) {
            ++found;
        }
    }
    return found;
}

/* Rebuild into the shared argv/storage; every role test compares the
 * argv the library would actually spawn. */
static size_t build(const krtsp_args_request *request)
{
    return krtsp_build_argv(request, argv, KRTSP_ARGV_MAX, storage,
                            sizeof(storage));
}

static bool
test_defaults(void)
{
    krtsp_args_request request;
    size_t count;

    krtsp_args_request_init(&request);
    CHECK(request.low_latency);
    CHECK(request.pixfmt == KRTSP_PIXFMT_RGBA);
    CHECK(!request.legacy_timeout_flag);

    request.url = sample_url;
    count = krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                             sizeof(storage));
    CHECK(count > 0u);
    CHECK(argv[count] == NULL);
    CHECK(strcmp(argv[0], "ffmpeg") == 0);

    /* The robustness flags are not optional; a camera fleet finds every
     * one of them eventually. */
    CHECK(has_pair(count, "-rtsp_transport", "tcp"));
    CHECK(has_pair(count, "-timeout", "10000000"));
    CHECK(has_pair(count, "-fflags", "+genpts+discardcorrupt"));
    CHECK(has_pair(count, "-avoid_negative_ts", "make_zero"));
    CHECK(has_pair(count, "-threads", "2"));
    CHECK(has_arg(count, "-nostdin"));

    /* Display, not recording: low latency on, no wallclock timestamps. */
    CHECK(has_pair(count, "-fflags", "nobuffer"));
    CHECK(has_pair(count, "-flags", "low_delay"));
    CHECK(!has_arg(count, "-use_wallclock_as_timestamps"));

    /* The consumer is a framebuffer presenter, not a detector. */
    CHECK(has_pair(count, "-f", "rawvideo"));
    CHECK(has_pair(count, "-pix_fmt", "rgba"));
    CHECK(has_arg(count, "-an"));
    CHECK(strcmp(argv[count - 1u], "-") == 0);

    /* The URL appears exactly once, and only as the -i operand. */
    CHECK(count_arg(count, sample_url) == 1u);
    CHECK(has_pair(count, "-i", sample_url));
    return true;
}

/*
 * A local input is not a camera and must not be handed camera options.
 *
 * ffmpeg exits 1 on a demuxer option the demuxer does not have, so the
 * unconditional -rtsp_transport meant a file could never be a source -
 * and a recording standing in for a camera is how everything built on
 * this gets tested without hardware.
 */
static bool
test_a_local_file_is_not_given_rtsp_options(void)
{
    krtsp_args_request request;
    size_t count;

    krtsp_args_request_init(&request);
    request.url = "/srv/footage/yesterday.mkv";
    count = krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                             sizeof(storage));
    CHECK(count > 0u);
    CHECK(!has_arg(count, "-rtsp_transport"));
    CHECK(!has_arg(count, "-timeout"));
    CHECK(!has_arg(count, "-stimeout"));
    CHECK(!has_arg(count, "-user_agent"));
    /* Paced by default, so the ring sees it at the rate it was shot. */
    CHECK(has_arg(count, "-re"));
    /* Everything that is not RTSP-specific still applies. */
    CHECK(has_pair(count, "-fflags", "+genpts+discardcorrupt"));
    CHECK(has_pair(count, "-avoid_negative_ts", "make_zero"));
    CHECK(has_pair(count, "-i", "/srv/footage/yesterday.mkv"));

    /* And scanning a file wants it as fast as it will go. */
    request.realtime = false;
    count = krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                             sizeof(storage));
    CHECK(count > 0u);
    CHECK(!has_arg(count, "-re"));

    /* rtsps is still a camera. */
    krtsp_args_request_init(&request);
    request.url = "rtsps://camera.example/stream";
    count = krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                             sizeof(storage));
    CHECK(has_pair(count, "-rtsp_transport", "tcp"));
    CHECK(!has_arg(count, "-re"));
    return true;
}

static bool
test_pixel_format_follows_the_consumer(void)
{
    krtsp_args_request request;
    size_t count;

    /* soft-raster's canvas is 0xAARRGGBB, which on little-endian is the
     * byte order B,G,R,A - so a frame headed for sr_canvas_wrap() must be
     * bgra.  Hardcoding either format gives a program that looks right in
     * one mode and channel-swapped in the other. */
    krtsp_args_request_init(&request);
    request.url = sample_url;
    request.pixfmt = KRTSP_PIXFMT_BGRA;
    count = krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                             sizeof(storage));
    CHECK(count > 0u);
    CHECK(has_pair(count, "-pix_fmt", "bgra"));
    CHECK(!has_pair(count, "-pix_fmt", "rgba"));
    return true;
}

static bool
test_scale_and_fps(void)
{
    krtsp_args_request request;
    size_t count;

    krtsp_args_request_init(&request);
    request.url = sample_url;

    /* Neither: no filter at all. */
    count = krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                             sizeof(storage));
    CHECK(count > 0u);
    CHECK(!has_arg(count, "-vf"));

    /* Scale only. */
    request.width = 320;
    request.height = 180;
    count = krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                             sizeof(storage));
    CHECK(count > 0u);
    CHECK(has_pair(count, "-vf", "scale=320:180"));

    /* Scale and cap. */
    request.fps_cap = 10;
    count = krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                             sizeof(storage));
    CHECK(count > 0u);
    CHECK(has_pair(count, "-vf", "scale=320:180,fps=10"));

    /* Cap only. */
    request.width = 0;
    request.height = 0;
    count = krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                             sizeof(storage));
    CHECK(count > 0u);
    CHECK(has_pair(count, "-vf", "fps=10"));
    return true;
}

static bool
test_low_latency_off(void)
{
    krtsp_args_request request;
    size_t count;

    krtsp_args_request_init(&request);
    request.url = sample_url;
    request.low_latency = false;
    count = krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                             sizeof(storage));
    CHECK(count > 0u);
    CHECK(!has_pair(count, "-fflags", "nobuffer"));
    CHECK(!has_pair(count, "-flags", "low_delay"));
    /* The robustness fflags survive; only the latency pair goes. */
    CHECK(has_pair(count, "-fflags", "+genpts+discardcorrupt"));
    return true;
}

static bool
test_legacy_timeout_flag(void)
{
    krtsp_args_request request;
    size_t count;

    /* ffmpeg renamed this at libavformat 59.  The reference install runs
     * a different build from the development machine, so it has to be
     * selectable rather than assumed. */
    krtsp_args_request_init(&request);
    request.url = sample_url;
    request.legacy_timeout_flag = true;
    count = krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                             sizeof(storage));
    CHECK(count > 0u);
    CHECK(has_pair(count, "-stimeout", "10000000"));
    CHECK(!has_arg(count, "-timeout"));
    return true;
}

static bool
test_rejections(void)
{
    krtsp_args_request request;
    char small_storage[16];
    char *small_argv[4];

    krtsp_args_request_init(&request);

    /* No URL. */
    CHECK(krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                           sizeof(storage)) == 0u);
    request.url = "";
    CHECK(krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                           sizeof(storage)) == 0u);

    /* A half-specified scale silently changes aspect ratio, and the
     * caller sizes frame buffers from these numbers. */
    request.url = sample_url;
    request.width = 320;
    request.height = 0;
    CHECK(krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                           sizeof(storage)) == 0u);

    /* Negatives. */
    request.width = -1;
    request.height = -1;
    CHECK(krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                           sizeof(storage)) == 0u);
    request.width = 0;
    request.height = 0;
    request.fps_cap = -5;
    CHECK(krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                           sizeof(storage)) == 0u);
    request.fps_cap = 0;

    /* Insufficient capacity fails rather than truncating: a truncated
     * argv is a subtly wrong ffmpeg invocation, not an obvious error. */
    CHECK(krtsp_build_argv(&request, small_argv, 4u, storage,
                           sizeof(storage)) == 0u);
    CHECK(krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, small_storage,
                           sizeof(small_storage)) == 0u);

    /* NULL arguments. */
    CHECK(krtsp_build_argv(NULL, argv, KRTSP_ARGV_MAX, storage,
                           sizeof(storage)) == 0u);
    CHECK(krtsp_build_argv(&request, NULL, KRTSP_ARGV_MAX, storage,
                           sizeof(storage)) == 0u);
    CHECK(krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, NULL,
                           sizeof(storage)) == 0u);
    return true;
}

static bool
test_url_redaction(void)
{
    char out[256];

    /* The password must not survive; the username may, because knowing
     * which account a camera uses is useful in a log and is not the
     * secret. */
    CHECK(krtsp_url_redact(sample_url, out, sizeof(out)));
    CHECK(strstr(out, "hunter2") == NULL);
    CHECK(strstr(out, "operator") != NULL);
    CHECK(strstr(out, "203.0.113.9") != NULL);
    CHECK(strcmp(out, "rtsp://operator:***@203.0.113.9:554/ch1") == 0);

    /* Userinfo with no password. */
    CHECK(krtsp_url_redact("rtsp://admin@203.0.113.9/1", out, sizeof(out)));
    CHECK(strcmp(out, "rtsp://admin@203.0.113.9/1") == 0);

    /* No userinfo at all: unchanged. */
    CHECK(krtsp_url_redact("rtsp://203.0.113.9:554/live", out, sizeof(out)));
    CHECK(strcmp(out, "rtsp://203.0.113.9:554/live") == 0);

    /* A password containing an @ must not end the userinfo early - the
     * LAST @ inside the authority is the delimiter.  Using the first one
     * leaks the tail of the password into what looks like the host, so
     * assert on every fragment, not just the whole string: checking only
     * for "p@ss" passes against an implementation that emits
     * "rtsp://u:***@ss@203.0.113.9/1". */
    CHECK(krtsp_url_redact("rtsp://u:p@ss@203.0.113.9/1", out, sizeof(out)));
    CHECK(strcmp(out, "rtsp://u:***@203.0.113.9/1") == 0);
    CHECK(strstr(out, "p@ss") == NULL);
    CHECK(strstr(out, "ss") == NULL);

    /* An '@' in the path is not userinfo and must not be treated as a
     * delimiter. */
    CHECK(krtsp_url_redact("rtsp://203.0.113.9/live@2", out, sizeof(out)));
    CHECK(strcmp(out, "rtsp://203.0.113.9/live@2") == 0);

    /* Userinfo plus an '@' later in the path. */
    CHECK(krtsp_url_redact("rtsp://u:pw@203.0.113.9/a@b", out, sizeof(out)));
    CHECK(strcmp(out, "rtsp://u:***@203.0.113.9/a@b") == 0);
    CHECK(strstr(out, "pw") == NULL);

    /* Too small: empty, never a truncated prefix that might still show
     * the start of a password. */
    CHECK(!krtsp_url_redact(sample_url, out, 8u));
    CHECK(out[0] == '\0');

    CHECK(!krtsp_url_redact(NULL, out, sizeof(out)));
    CHECK(out[0] == '\0');
    return true;
}

static bool
test_password_escaping(void)
{
    char out[512];

    /* An unescaped '@' in a password ends the userinfo early: ffmpeg
     * parses "ss@203.0.113.9" as the host and reports a connection error
     * naming somewhere that does not exist.  That reads as a network
     * fault, which is why this has to be handled rather than diagnosed. */
    CHECK(krtsp_url_escape_password("rtsp://u:p@ss@203.0.113.9/1", out,
                                    sizeof(out)));
    CHECK(strcmp(out, "rtsp://u:p%40ss@203.0.113.9/1") == 0);

    /* ':' is recoverable: the first ':' in the authority separates user
     * from password, so any later one is part of the password. */
    CHECK(krtsp_url_escape_password("rtsp://u:a:b@203.0.113.9/1", out,
                                    sizeof(out)));
    CHECK(strcmp(out, "rtsp://u:a%3Ab@203.0.113.9/1") == 0);

    /*
     * '/' '?' and '#' are NOT recoverable, and pretending otherwise
     * would be worse than leaving them alone.  Each ends the authority,
     * so "rtsp://u:a/b@host/1" genuinely parses as user "u", no
     * password, host "u", path "/b@host/1" - Python's urlsplit and every
     * other conforming parser agree.  No amount of cleverness can tell
     * that apart from a URL that really does have a path.
     *
     * So the URL is passed through unchanged and the operator must
     * percent-encode those characters in the configuration file.  The
     * alternative - scanning for the last '@' anywhere in the string -
     * misparses "rtsp://host:554/live@2" as a password of "554/live",
     * which is a real URL shape.
     */
    CHECK(krtsp_url_escape_password("rtsp://u:a/b@203.0.113.9/1", out,
                                    sizeof(out)));
    CHECK(strcmp(out, "rtsp://u:a/b@203.0.113.9/1") == 0);
    CHECK(krtsp_url_escape_password("rtsp://u:a?c@203.0.113.9/1", out,
                                    sizeof(out)));
    CHECK(strcmp(out, "rtsp://u:a?c@203.0.113.9/1") == 0);

    /* A host:port followed by a path containing '@' must never be read
     * as userinfo. */
    CHECK(krtsp_url_escape_password("rtsp://host:554/live@2", out,
                                    sizeof(out)));
    CHECK(strcmp(out, "rtsp://host:554/live@2") == 0);

    /* Unreserved characters are left alone, so an ordinary password stays
     * readable in a process listing and in a log. */
    CHECK(krtsp_url_escape_password("rtsp://u:Aa0-._~@203.0.113.9/1", out,
                                    sizeof(out)));
    CHECK(strcmp(out, "rtsp://u:Aa0-._~@203.0.113.9/1") == 0);

    /* A space becomes %20, not '+': in userinfo a '+' is a literal plus,
     * so form encoding would silently change the password. */
    CHECK(krtsp_url_escape_password("rtsp://u:a b@203.0.113.9/1", out,
                                    sizeof(out)));
    CHECK(strcmp(out, "rtsp://u:a%20b@203.0.113.9/1") == 0);

    /* Configuration must percent-encode '/', '?' and '#'.  Preserve valid
     * triplets: encoding '%' again silently changes the actual password. */
    CHECK(krtsp_url_escape_password(
        "rtsp://u:a%2Fb%3Fc%23d@203.0.113.9/1", out, sizeof(out)));
    CHECK(strcmp(out,
                 "rtsp://u:a%2Fb%3Fc%23d@203.0.113.9/1") == 0);
    /* A malformed triplet is a literal percent and must still be escaped. */
    CHECK(krtsp_url_escape_password("rtsp://u:a%2G@203.0.113.9/1", out,
                                    sizeof(out)));
    CHECK(strcmp(out, "rtsp://u:a%252G@203.0.113.9/1") == 0);

    /* Nothing to do when there is no password, or no userinfo at all. */
    CHECK(krtsp_url_escape_password("rtsp://admin@203.0.113.9/1", out,
                                    sizeof(out)));
    CHECK(strcmp(out, "rtsp://admin@203.0.113.9/1") == 0);
    CHECK(krtsp_url_escape_password("rtsp://203.0.113.9/live", out,
                                    sizeof(out)));
    CHECK(strcmp(out, "rtsp://203.0.113.9/live") == 0);

    /* An '@' in the path is not userinfo. */
    CHECK(krtsp_url_escape_password("rtsp://203.0.113.9/live@2", out,
                                    sizeof(out)));
    CHECK(strcmp(out, "rtsp://203.0.113.9/live@2") == 0);

    /* Too small fails empty rather than emitting a half-escaped URL,
     * which would connect somewhere unintended. */
    CHECK(!krtsp_url_escape_password("rtsp://u:p@ss@203.0.113.9/1", out, 12u));
    CHECK(out[0] == '\0');
    CHECK(!krtsp_url_escape_password(NULL, out, sizeof(out)));
    return true;
}

static bool
test_argv_escapes_the_url(void)
{
    krtsp_args_request request;
    size_t count;
    bool found = false;

    /* The builder must apply escaping itself: a caller that forgets gets
     * a confusing connection error rather than an obvious mistake. */
    krtsp_args_request_init(&request);
    request.url = "rtsp://u:p@ss@203.0.113.9:554/ch1";
    count = krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                             sizeof(storage));
    CHECK(count > 0u);
    CHECK(has_pair(count, "-i", "rtsp://u:p%40ss@203.0.113.9:554/ch1"));

    /* The raw form must not survive anywhere in the argv. */
    for (size_t index = 0u; index < count; ++index) {
        if (strcmp(argv[index], request.url) == 0) {
            found = true;
        }
    }
    CHECK(!found);
    return true;
}

/* The record sink exists so one ffmpeg, on one RTSP session, can feed a
 * viewer and write an archive.  These pin the argv it produces. */
static bool
test_record_role_adds_a_copy_sink(void)
{
    krtsp_args_request request;
    size_t count;

    krtsp_args_request_init(&request);
    request.url = sample_url;
    request.width = 640;
    request.height = 360;
    request.roles = (unsigned)KRTSP_ROLE_DECODE | (unsigned)KRTSP_ROLE_RECORD;
    request.record_dir = "/tmp/rec";
    count = build(&request);
    CHECK(count > 0u);

    /* Both sinks on one input: the decode pipe survives untouched... */
    CHECK(has_pair(count, "-f", "rawvideo"));
    CHECK(has_arg(count, "-"));
    /* ...and the archive copies rather than re-encodes, which is the
     * whole point: re-encoding at the same resolution measurably
     * produces larger files for a core's worth of CPU. */
    CHECK(has_pair(count, "-c", "copy"));
    CHECK(has_pair(count, "-f", "segment"));
    CHECK(has_pair(count, "-segment_time", "10"));
    CHECK(has_pair(count, "-reset_timestamps", "1"));
    CHECK(has_pair(count, "-strftime", "1"));
    CHECK(has_pair(count, "-map", "0:v"));
    /* one input only, so exactly one -i */
    CHECK(count_arg(count, "-i") == 1u);
    return true;
}

static bool
test_record_only_never_decodes(void)
{
    krtsp_args_request request;
    size_t count;

    krtsp_args_request_init(&request);
    request.url = sample_url;
    request.roles = (unsigned)KRTSP_ROLE_RECORD;
    request.record_dir = "/tmp/rec";
    count = build(&request);
    CHECK(count > 0u);

    /* No decode sink at all: this camera costs I/O and nothing else. */
    CHECK(!has_pair(count, "-f", "rawvideo"));
    CHECK(!has_arg(count, "-pix_fmt"));
    CHECK(!has_arg(count, "-vf"));
    CHECK(has_pair(count, "-f", "segment"));

    /* Low-latency input flags trade buffering for promptness, which an
     * archive has no use for and something to lose by. */
    CHECK(!has_pair(count, "-fflags", "nobuffer"));
    CHECK(!has_pair(count, "-flags", "low_delay"));
    return true;
}

static bool
test_record_audio_and_container(void)
{
    krtsp_args_request request;
    size_t count;

    krtsp_args_request_init(&request);
    request.url = sample_url;
    request.roles = (unsigned)KRTSP_ROLE_RECORD;
    request.record_dir = "/tmp/rec";
    count = build(&request);
    CHECK(count > 0u);
    /* Video only by default: pairing audio with the wrong container fails
     * the whole process rather than just the audio. */
    CHECK(has_pair(count, "-map", "0:v"));
    CHECK(!has_pair(count, "-map", "0:a?"));
    /* Default pattern is Matroska and flat, both deliberately: mp4 cannot
     * mux the pcm_alaw these cameras carry, and a flat pattern needs no
     * directory creation, which not every ffmpeg can do. */
    CHECK(has_arg(count, "/tmp/rec/%Y-%m-%d_%H.%M.%S.mkv"));

    request.record_audio = true;
    count = build(&request);
    CHECK(count > 0u);
    /* '?' so a camera without an audio track still records. */
    CHECK(has_pair(count, "-map", "0:a?"));

    request.record_pattern = "%Y/%m/%d.mkv";
    request.segment_seconds = 30;
    count = build(&request);
    CHECK(count > 0u);
    CHECK(has_arg(count, "/tmp/rec/%Y/%m/%d.mkv"));
    CHECK(has_pair(count, "-segment_time", "30"));
    return true;
}

/* -strftime_mkdir does not exist in every ffmpeg - 5.1 lacks it - and
 * passing a flag the binary does not know fails the spawn outright.  It
 * is emitted only when the caller reports the binary has it. */
static bool
test_segment_mkdir_is_opt_in(void)
{
    krtsp_args_request request;
    size_t count;

    krtsp_args_request_init(&request);
    request.url = sample_url;
    request.roles = (unsigned)KRTSP_ROLE_RECORD;
    request.record_dir = "/tmp/rec";
    count = build(&request);
    CHECK(count > 0u);
    CHECK(!has_arg(count, "-strftime_mkdir"));

    request.segment_mkdir = true;
    count = build(&request);
    CHECK(count > 0u);
    CHECK(has_pair(count, "-strftime_mkdir", "1"));
    return true;
}

static bool
test_segment_list_is_opt_in(void)
{
    krtsp_args_request request;
    size_t count;

    krtsp_args_request_init(&request);
    request.url = sample_url;
    request.roles = (unsigned)KRTSP_ROLE_RECORD;
    request.record_dir = "/tmp/rec";
    count = build(&request);
    CHECK(count > 0u);
    CHECK(!has_arg(count, "-segment_list"));

    request.segment_list = "/tmp/rec/.segments";
    count = build(&request);
    CHECK(count > 0u);
    CHECK(has_pair(count, "-segment_list", "/tmp/rec/.segments"));
    /* Bounded: a supervisor stats this for liveness, and it must not grow
     * without limit over a month of uptime. */
    CHECK(has_pair(count, "-segment_list_size", "8"));
    CHECK(has_pair(count, "-segment_list_flags", "+live"));
    return true;
}

static bool
test_role_rejections(void)
{
    krtsp_args_request request;

    krtsp_args_request_init(&request);
    request.url = sample_url;
    request.width = 640;
    request.height = 360;

    /* A recording sink with nowhere to write would spawn a process
     * guaranteed to fail; refusing reports it as the configuration error
     * it is. */
    request.roles = (unsigned)KRTSP_ROLE_RECORD;
    request.record_dir = NULL;
    CHECK(build(&request) == 0u);
    request.record_dir = "";
    CHECK(build(&request) == 0u);

    request.record_dir = "/tmp/rec";
    request.segment_seconds = -1;
    CHECK(build(&request) == 0u);
    request.segment_seconds = 0;

    /* An unknown role bit is a caller error, not something to ignore. */
    request.roles = 0x80u;
    CHECK(build(&request) == 0u);

    /* Zero means decode, which is what every caller predating the record
     * sink asked for. */
    request.roles = 0u;
    CHECK(build(&request) > 0u);
    CHECK(has_pair(build(&request), "-f", "rawvideo"));
    return true;
}

typedef bool (*test_function)(void);

typedef struct test_case {
    const char *name;
    test_function function;
} test_case;

int
main(void)
{
    static const test_case tests[] = {
        {"defaults", test_defaults},
        {"a local file is not given rtsp options",
         test_a_local_file_is_not_given_rtsp_options},
        {"pixel format follows the consumer",
         test_pixel_format_follows_the_consumer},
        {"scale and fps", test_scale_and_fps},
        {"low latency off", test_low_latency_off},
        {"legacy timeout flag", test_legacy_timeout_flag},
        {"rejections", test_rejections},
        {"record role adds a copy sink", test_record_role_adds_a_copy_sink},
        {"record only never decodes", test_record_only_never_decodes},
        {"record audio and container", test_record_audio_and_container},
        {"segment mkdir is opt in", test_segment_mkdir_is_opt_in},
        {"segment list is opt in", test_segment_list_is_opt_in},
        {"role rejections", test_role_rejections},
        {"url redaction", test_url_redaction},
        {"password escaping", test_password_escaping},
        {"argv escapes the url", test_argv_escapes_the_url}
    };
    size_t passed = 0u;

    for (size_t index = 0u; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        const bool ok = tests[index].function();

        (void)printf("%s %s\n", ok ? "ok" : "not ok", tests[index].name);
        if (!ok) {
            return 1;
        }
        ++passed;
    }
    (void)printf("%zu tests passed\n", passed);
    return 0;
}
