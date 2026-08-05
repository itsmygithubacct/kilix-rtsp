#include "kilix_rtsp.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

enum { W = 8, H = 4 };

static void sleep_ms(int milliseconds)
{
    struct timespec pause;

    pause.tv_sec = milliseconds / 1000;
    pause.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    (void)nanosleep(&pause, NULL);
}

static void fill(uint8_t *buffer, size_t size, uint8_t value)
{
    memset(buffer, value, size);
}

static bool
test_lifecycle(void)
{
    krtsp_frame *frame = NULL;
    const uint8_t *pixels;
    uint64_t sequence = 0u;
    uint64_t published = 0u;
    uint64_t dropped = 0u;
    int age = -1;

    CHECK(krtsp_frame_init(&frame, W, H));
    CHECK(frame != NULL);
    CHECK(krtsp_frame_size(frame) == (size_t)W * H * 4u);
    CHECK(krtsp_frame_back(frame) != NULL);

    /* Nothing published yet: a borrow must report that rather than
     * handing back a zeroed buffer the caller would render as black. */
    CHECK(krtsp_frame_borrow(frame, &sequence, &age) == NULL);

    fill(krtsp_frame_back(frame), krtsp_frame_size(frame), 0x41u);
    krtsp_frame_publish(frame, NULL);

    pixels = krtsp_frame_borrow(frame, &sequence, &age);
    CHECK(pixels != NULL);
    CHECK(sequence == 1u);
    CHECK(age >= 0 && age < 1000);
    CHECK(pixels[0] == 0x41u);
    CHECK(pixels[krtsp_frame_size(frame) - 1u] == 0x41u);

    /* A second borrow while one is outstanding is refused rather than
     * handing two consumers the same buffer. */
    CHECK(krtsp_frame_borrow(frame, NULL, NULL) == NULL);
    krtsp_frame_release(frame);

    krtsp_frame_stats(frame, &published, &dropped);
    CHECK(published == 1u);
    CHECK(dropped == 0u);

    krtsp_frame_free(frame);
    return true;
}

static bool
test_newest_wins(void)
{
    krtsp_frame *frame = NULL;
    const uint8_t *pixels;
    uint64_t sequence = 0u;
    uint64_t published = 0u;
    uint64_t dropped = 0u;
    bool was_dropped = false;

    CHECK(krtsp_frame_init(&frame, W, H));

    /* Three frames with no consumer in between: the slow consumer must
     * see the NEWEST, not the oldest queued one.  A queue here would
     * trade the only thing a live view is for. */
    fill(krtsp_frame_back(frame), krtsp_frame_size(frame), 1u);
    krtsp_frame_publish(frame, &was_dropped);
    CHECK(!was_dropped);

    fill(krtsp_frame_back(frame), krtsp_frame_size(frame), 2u);
    krtsp_frame_publish(frame, &was_dropped);
    CHECK(was_dropped);

    fill(krtsp_frame_back(frame), krtsp_frame_size(frame), 3u);
    krtsp_frame_publish(frame, &was_dropped);
    CHECK(was_dropped);

    pixels = krtsp_frame_borrow(frame, &sequence, NULL);
    CHECK(pixels != NULL);
    CHECK(pixels[0] == 3u);
    CHECK(sequence == 3u);
    krtsp_frame_release(frame);

    krtsp_frame_stats(frame, &published, &dropped);
    CHECK(published == 3u);
    CHECK(dropped == 2u);

    krtsp_frame_free(frame);
    return true;
}

static bool
test_publish_during_borrow(void)
{
    krtsp_frame *frame = NULL;
    const uint8_t *pixels;
    uint64_t sequence = 0u;

    CHECK(krtsp_frame_init(&frame, W, H));

    fill(krtsp_frame_back(frame), krtsp_frame_size(frame), 10u);
    krtsp_frame_publish(frame, NULL);

    pixels = krtsp_frame_borrow(frame, &sequence, NULL);
    CHECK(pixels != NULL);
    CHECK(pixels[0] == 10u);

    /* Publishing while the consumer holds the front buffer must not swap
     * it out from under the reader.  The borrowed pixels must stay
     * stable for the whole borrow. */
    fill(krtsp_frame_back(frame), krtsp_frame_size(frame), 20u);
    krtsp_frame_publish(frame, NULL);
    CHECK(pixels[0] == 10u);

    krtsp_frame_release(frame);

    /* ...and the frame that arrived mid-borrow is delivered next, rather
     * than being lost or making the consumer wait for another. */
    pixels = krtsp_frame_borrow(frame, &sequence, NULL);
    CHECK(pixels != NULL);
    CHECK(pixels[0] == 20u);
    CHECK(sequence == 2u);
    krtsp_frame_release(frame);

    krtsp_frame_free(frame);
    return true;
}

static bool
test_age_reports_staleness(void)
{
    krtsp_frame *frame = NULL;
    int age = -1;

    CHECK(krtsp_frame_init(&frame, W, H));
    fill(krtsp_frame_back(frame), krtsp_frame_size(frame), 7u);
    krtsp_frame_publish(frame, NULL);

    sleep_ms(120);

    /* A frozen camera keeps delivering a valid frame forever, so age is
     * the only signal that distinguishes it from a still scene.  This is
     * what the viewer's staleness badge is built on. */
    CHECK(krtsp_frame_borrow(frame, NULL, &age) != NULL);
    CHECK(age >= 100);
    krtsp_frame_release(frame);

    krtsp_frame_free(frame);
    return true;
}

/* ------------------------- concurrency ---------------------------------- */

typedef struct hammer {
    krtsp_frame *frame;
    int iterations;
    int torn;
} hammer;

static void *producer_main(void *argument)
{
    hammer *state = argument;

    for (int index = 0; index < state->iterations; ++index) {
        uint8_t value = (uint8_t)((index % 254) + 1);

        fill(krtsp_frame_back(state->frame),
             krtsp_frame_size(state->frame), value);
        krtsp_frame_publish(state->frame, NULL);
    }
    return NULL;
}

static void *consumer_main(void *argument)
{
    hammer *state = argument;
    size_t size = krtsp_frame_size(state->frame);

    for (int index = 0; index < state->iterations; ++index) {
        const uint8_t *pixels = krtsp_frame_borrow(state->frame, NULL, NULL);

        if (pixels == NULL) {
            continue;
        }
        /* Every byte of a borrowed frame must come from one publish.  A
         * mixture means the buffer was swapped or written during the
         * borrow, which is the bug this whole design exists to avoid. */
        uint8_t first = pixels[0];
        for (size_t at = 1u; at < size; ++at) {
            if (pixels[at] != first) {
                state->torn++;
                break;
            }
        }
        krtsp_frame_release(state->frame);
    }
    return NULL;
}

static bool
test_concurrent_publish_and_borrow(void)
{
    krtsp_frame *frame = NULL;
    pthread_t producer;
    pthread_t consumer;
    hammer state;

    CHECK(krtsp_frame_init(&frame, 64, 64));
    state.frame = frame;
    state.iterations = 4000;
    state.torn = 0;

    CHECK(pthread_create(&producer, NULL, producer_main, &state) == 0);
    CHECK(pthread_create(&consumer, NULL, consumer_main, &state) == 0);
    CHECK(pthread_join(producer, NULL) == 0);
    CHECK(pthread_join(consumer, NULL) == 0);

    CHECK(state.torn == 0);

    krtsp_frame_free(frame);
    return true;
}

static bool
test_rejections(void)
{
    krtsp_frame *frame = NULL;

    CHECK(!krtsp_frame_init(&frame, 0, 4));
    CHECK(frame == NULL);
    CHECK(!krtsp_frame_init(&frame, 4, 0));
    CHECK(!krtsp_frame_init(&frame, -1, -1));
    CHECK(!krtsp_frame_init(NULL, 4, 4));

    /* NULL is a safe no-op everywhere, so teardown paths need no guards. */
    CHECK(krtsp_frame_back(NULL) == NULL);
    CHECK(krtsp_frame_size(NULL) == 0u);
    CHECK(krtsp_frame_borrow(NULL, NULL, NULL) == NULL);
    krtsp_frame_publish(NULL, NULL);
    krtsp_frame_release(NULL);
    krtsp_frame_stats(NULL, NULL, NULL);
    krtsp_frame_free(NULL);
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
        {"lifecycle", test_lifecycle},
        {"newest wins", test_newest_wins},
        {"publish during borrow", test_publish_during_borrow},
        {"age reports staleness", test_age_reports_staleness},
        {"concurrent publish and borrow", test_concurrent_publish_and_borrow},
        {"rejections", test_rejections}
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
