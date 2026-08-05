/*
 * Process-supervision tests, driven by tests/fake_ffmpeg.c rather than by
 * a camera.  Every failure mode below is one a real camera produces
 * eventually and none of them can be produced on demand, which is why the
 * stand-in exists.
 */

#include "kilix_rtsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

enum { W = 8, H = 4 };

static const char *fake_path(void)
{
    const char *override = getenv("KRTSP_FAKE_FFMPEG");

    return override != NULL ? override : "build/fake-ffmpeg";
}

static void sleep_ms(int milliseconds)
{
    struct timespec pause;

    pause.tv_sec = milliseconds / 1000;
    pause.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    (void)nanosleep(&pause, NULL);
}

static void configure_fake(const char *mode, int frames, int delay_ms)
{
    char buffer[32];

    (void)setenv("FAKE_FFMPEG_MODE", mode, 1);
    (void)snprintf(buffer, sizeof(buffer), "%d", W);
    (void)setenv("FAKE_FFMPEG_WIDTH", buffer, 1);
    (void)snprintf(buffer, sizeof(buffer), "%d", H);
    (void)setenv("FAKE_FFMPEG_HEIGHT", buffer, 1);
    (void)snprintf(buffer, sizeof(buffer), "%d", frames);
    (void)setenv("FAKE_FFMPEG_FRAMES", buffer, 1);
    (void)snprintf(buffer, sizeof(buffer), "%d", delay_ms);
    (void)setenv("FAKE_FFMPEG_DELAY_MS", buffer, 1);
    (void)unsetenv("FAKE_FFMPEG_STARTUP_MS");
}

static void base_options(krtsp_source_options *options)
{
    krtsp_source_options_init(options);
    options->width = W;
    options->height = H;
    options->ffmpeg_path = fake_path();
    /* Compressed timings so the suite runs in seconds rather than in the
     * tens of seconds a real camera's tolerances would need. */
    options->stall_ms = 400;
    options->grace_ms = 200;
    options->backoff_min_ms = 100;
    options->backoff_max_ms = 400;
    options->stable_ms = 100000;   /* never "stable" unless a test says so */
}

/* Wait for a predicate, so tests do not depend on scheduler timing. */
static bool wait_for_status(krtsp_source *source, krtsp_status want, int ms)
{
    int waited = 0;

    while (waited < ms) {
        if (krtsp_source_status(source) == want) {
            return true;
        }
        sleep_ms(20);
        waited += 20;
    }
    return krtsp_source_status(source) == want;
}

static bool wait_for_frames(krtsp_source *source, uint64_t want, int ms)
{
    krtsp_source_stats stats;
    int waited = 0;

    while (waited < ms) {
        krtsp_source_get_stats(source, &stats);
        if (stats.frames >= want) {
            return true;
        }
        sleep_ms(20);
        waited += 20;
    }
    krtsp_source_get_stats(source, &stats);
    return stats.frames >= want;
}

static bool wait_for_restarts(krtsp_source *source, uint64_t want, int ms)
{
    krtsp_source_stats stats;
    int waited = 0;

    while (waited < ms) {
        krtsp_source_get_stats(source, &stats);
        if (stats.restarts >= want) {
            return true;
        }
        sleep_ms(20);
        waited += 20;
    }
    krtsp_source_get_stats(source, &stats);
    return stats.restarts >= want;
}

/* ------------------------------- the tests ------------------------------ */

static bool
test_frames_arrive(void)
{
    krtsp_source *source = NULL;
    krtsp_source_options options;
    const uint8_t *pixels;
    int age = -1;

    configure_fake("normal", -1, 10);
    base_options(&options);

    CHECK(krtsp_source_start(&source, "rtsp://u:p@example.invalid/1",
                             &options));
    CHECK(wait_for_frames(source, 3u, 3000));
    CHECK(wait_for_status(source, KRTSP_ONLINE, 1000));

    pixels = krtsp_source_borrow(source, &age);
    CHECK(pixels != NULL);
    CHECK(age >= 0 && age < 2000);
    /* fake-ffmpeg fills each frame with one repeated byte, so a torn or
     * misaligned frame shows up immediately. */
    for (size_t at = 1u; at < (size_t)W * H * 4u; ++at) {
        CHECK(pixels[at] == pixels[0]);
    }
    krtsp_source_release(source);

    krtsp_source_stop(source);
    return true;
}

static bool
test_child_exit_restarts(void)
{
    krtsp_source *source = NULL;
    krtsp_source_options options;
    krtsp_source_stats stats;

    /* Emit two frames then exit non-zero, repeatedly. */
    configure_fake("die", 2, 10);
    base_options(&options);

    CHECK(krtsp_source_start(&source, "rtsp://u:p@example.invalid/1",
                             &options));
    CHECK(wait_for_restarts(source, 2u, 6000));

    krtsp_source_get_stats(source, &stats);
    CHECK(stats.restarts >= 2u);
    CHECK(stats.frames >= 4u);   /* two per run */
    /* An exit is not a stall; conflating them hides which failure is
     * actually happening on a camera. */
    CHECK(stats.stalls == 0u);

    krtsp_source_stop(source);
    return true;
}

static bool
test_wedged_camera_is_detected(void)
{
    krtsp_source *source = NULL;
    krtsp_source_options options;
    krtsp_source_stats stats;

    /* The failure that matters: two frames, then hold the pipe open
     * forever without sending anything.  The socket stays valid, read()
     * blocks with no error, and nothing but a timer notices. */
    configure_fake("silent", 2, 10);
    base_options(&options);

    CHECK(krtsp_source_start(&source, "rtsp://u:p@example.invalid/1",
                             &options));
    CHECK(wait_for_frames(source, 2u, 3000));

    /* grace 200ms + stall 400ms, so a restart is due within ~1s. */
    CHECK(wait_for_restarts(source, 1u, 5000));

    krtsp_source_get_stats(source, &stats);
    CHECK(stats.stalls >= 1u);
    CHECK(stats.restarts >= 1u);

    krtsp_source_stop(source);
    return true;
}

static bool
test_grace_period_protects_slow_start(void)
{
    krtsp_source *source = NULL;
    krtsp_source_options options;
    krtsp_source_stats stats;

    /* A camera that takes a while to produce its first frame must not be
     * killed for it.  Without a grace period this restarts forever and
     * never delivers anything. */
    configure_fake("normal", -1, 10);
    (void)setenv("FAKE_FFMPEG_STARTUP_MS", "500", 1);

    base_options(&options);
    options.grace_ms = 2000;
    options.stall_ms = 300;

    CHECK(krtsp_source_start(&source, "rtsp://u:p@example.invalid/1",
                             &options));
    CHECK(wait_for_frames(source, 2u, 4000));

    krtsp_source_get_stats(source, &stats);
    CHECK(stats.stalls == 0u);
    CHECK(stats.restarts == 0u);

    krtsp_source_stop(source);
    (void)unsetenv("FAKE_FFMPEG_STARTUP_MS");
    return true;
}

static bool
test_partial_frame_resumes(void)
{
    krtsp_source *source = NULL;
    krtsp_source_options options;
    const uint8_t *pixels;

    /* A pipe splits writes wherever it likes.  A reader that discards the
     * remainder of a short read is misaligned for every later frame, and
     * the result looks like a codec bug rather than a framing error. */
    configure_fake("partial", 2, 10);
    base_options(&options);

    CHECK(krtsp_source_start(&source, "rtsp://u:p@example.invalid/1",
                             &options));
    CHECK(wait_for_frames(source, 5u, 5000));

    pixels = krtsp_source_borrow(source, NULL);
    CHECK(pixels != NULL);
    /* Still uniform: alignment survived the split write. */
    for (size_t at = 1u; at < (size_t)W * H * 4u; ++at) {
        CHECK(pixels[at] == pixels[0]);
    }
    krtsp_source_release(source);

    krtsp_source_stop(source);
    return true;
}

static bool
test_spawn_failure_backs_off_and_gives_up(void)
{
    krtsp_source *source = NULL;
    krtsp_source_options options;
    krtsp_source_stats stats;

    /* A binary that cannot start at all: the source must keep reporting
     * a state rather than hanging, and must stop trying when told to. */
    configure_fake("startfail", 0, 10);
    base_options(&options);
    options.max_consecutive_failures = 3;

    CHECK(krtsp_source_start(&source, "rtsp://u:p@example.invalid/1",
                             &options));
    CHECK(wait_for_status(source, KRTSP_FAILED, 6000));

    krtsp_source_get_stats(source, &stats);
    CHECK(stats.consecutive_failures >= 3);
    CHECK(stats.frames == 0u);

    /* A source with nothing to show reports it rather than handing back
     * a black frame. */
    CHECK(krtsp_source_borrow(source, NULL) == NULL);

    krtsp_source_stop(source);
    return true;
}

static bool
test_stop_while_wedged(void)
{
    krtsp_source *source = NULL;
    krtsp_source_options options;

    /* Teardown must not deadlock on the case it exists for: the reader is
     * blocked in read() on a camera that will never send again.  Joining
     * the reader before killing the child would hang here forever. */
    configure_fake("silent", 1, 10);
    base_options(&options);
    options.stall_ms = 100000;   /* the watchdog must NOT be what saves us */
    options.grace_ms = 100000;

    CHECK(krtsp_source_start(&source, "rtsp://u:p@example.invalid/1",
                             &options));
    CHECK(wait_for_frames(source, 1u, 3000));

    krtsp_source_stop(source);   /* hangs forever if the order is wrong */
    return true;
}

static bool
test_missing_binary_is_not_a_start_failure(void)
{
    krtsp_source *source = NULL;
    krtsp_source_options options;

    /* An unreachable camera is a runtime state, not a construction
     * error: a wall display should come up and keep retrying rather than
     * refusing to start. */
    base_options(&options);
    options.ffmpeg_path = "./definitely-not-a-real-binary";
    options.max_consecutive_failures = 2;

    CHECK(krtsp_source_start(&source, "rtsp://u:p@example.invalid/1",
                             &options));
    CHECK(wait_for_status(source, KRTSP_FAILED, 6000));
    krtsp_source_stop(source);
    return true;
}

static bool
test_rejections(void)
{
    krtsp_source *source = NULL;
    krtsp_source_options options;

    base_options(&options);

    CHECK(!krtsp_source_start(NULL, "rtsp://x/1", &options));
    CHECK(!krtsp_source_start(&source, NULL, &options));
    CHECK(!krtsp_source_start(&source, "", &options));

    options.width = 0;
    CHECK(!krtsp_source_start(&source, "rtsp://x/1", &options));
    base_options(&options);

    options.backoff_max_ms = 1;   /* below the minimum */
    CHECK(!krtsp_source_start(&source, "rtsp://x/1", &options));
    base_options(&options);

    options.stall_ms = 0;
    CHECK(!krtsp_source_start(&source, "rtsp://x/1", &options));

    /* NULL is a safe no-op on every teardown path. */
    krtsp_source_stop(NULL);
    krtsp_source_release(NULL);
    CHECK(krtsp_source_borrow(NULL, NULL) == NULL);
    CHECK(krtsp_source_status(NULL) == KRTSP_FAILED);
    CHECK(strcmp(krtsp_status_name(KRTSP_ONLINE), "online") == 0);
    CHECK(strcmp(krtsp_status_name(KRTSP_STALE), "stale") == 0);
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
        {"frames arrive", test_frames_arrive},
        {"child exit restarts", test_child_exit_restarts},
        {"wedged camera is detected", test_wedged_camera_is_detected},
        {"grace period protects slow start",
         test_grace_period_protects_slow_start},
        {"partial frame resumes", test_partial_frame_resumes},
        {"spawn failure backs off and gives up",
         test_spawn_failure_backs_off_and_gives_up},
        {"stop while wedged", test_stop_while_wedged},
        {"missing binary is not a start failure",
         test_missing_binary_is_not_a_start_failure},
        {"rejections", test_rejections}
    };
    size_t passed = 0u;

    if (access(fake_path(), X_OK) != 0) {
        (void)fprintf(stderr,
                      "fake ffmpeg not found at %s; run make test\n",
                      fake_path());
        return 1;
    }

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
