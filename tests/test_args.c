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
        {"pixel format follows the consumer",
         test_pixel_format_follows_the_consumer},
        {"scale and fps", test_scale_and_fps},
        {"low latency off", test_low_latency_off},
        {"legacy timeout flag", test_legacy_timeout_flag},
        {"rejections", test_rejections},
        {"url redaction", test_url_redaction}
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
