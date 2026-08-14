/*
 * Throughput of the frame ring: the handoff every decoded frame crosses,
 * with a producer and a borrower contending the way the reader thread
 * and the present loop do.
 *
 * Numbers, not thresholds.  Runs are compared across commits by hand;
 * a threshold tight enough to catch a regression is loose only on the
 * machine it was tuned on, and a flaky red is worse than none.
 */

#include "kilix_rtsp.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FRAME_W 1920
#define FRAME_H 1080

static double monotonic_seconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1.0;
    }
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

typedef struct contention {
    krtsp_frame *producer;
    krtsp_frame *consumer;
    size_t publishes;
    bool full_fill;         /* write the whole frame, as a decoder would */
    atomic_bool done;
    uint64_t borrows;
    uint64_t dropped;
    uint64_t sink;          /* keeps the borrowed reads alive */
} contention;

static void *producer_main(void *opaque)
{
    contention *state = opaque;
    const size_t bytes = krtsp_frame_size(state->producer);

    for (size_t index = 0u; index < state->publishes; ++index) {
        uint8_t *back = krtsp_frame_back(state->producer);
        bool dropped = false;

        if (back == NULL) {
            break;
        }
        (void)memset(back, (int)(index & 0xFFu),
                     state->full_fill ? bytes : 64u);
        krtsp_frame_publish(state->producer, &dropped);
        if (dropped) {
            ++state->dropped;
        }
    }
    atomic_store(&state->done, true);
    return NULL;
}

static void *consumer_main(void *opaque)
{
    contention *state = opaque;

    while (!atomic_load(&state->done)) {
        uint64_t sequence = 0u;
        int age_ms = 0;
        const uint8_t *pixels =
            krtsp_frame_borrow(state->consumer, &sequence, &age_ms);

        if (pixels != NULL) {
            state->sink += pixels[0];
            ++state->borrows;
            krtsp_frame_release(state->consumer);
        }
    }
    return NULL;
}

static bool run_contention(const char *name, krtsp_frame *producer,
                           krtsp_frame *consumer, size_t publishes,
                           bool full_fill)
{
    contention state;
    pthread_t producing;
    pthread_t consuming;
    double started;
    double elapsed;

    (void)memset(&state, 0, sizeof(state));
    state.producer = producer;
    state.consumer = consumer;
    state.publishes = publishes;
    state.full_fill = full_fill;
    atomic_init(&state.done, false);

    started = monotonic_seconds();
    if (started < 0.0 ||
        pthread_create(&producing, NULL, producer_main, &state) != 0) {
        return false;
    }
    if (pthread_create(&consuming, NULL, consumer_main, &state) != 0) {
        atomic_store(&state.done, true);
        (void)pthread_join(producing, NULL);
        return false;
    }
    (void)pthread_join(producing, NULL);
    (void)pthread_join(consuming, NULL);
    elapsed = monotonic_seconds() - started;
    if (!(elapsed > 0.0)) {
        return false;
    }
    (void)printf(
        "%s publishes=%zu borrows=%" PRIu64 " dropped=%" PRIu64
        " publishes_s=%.0f mean_us=%.2f sink=%" PRIu64 "\n",
        name, publishes, state.borrows, state.dropped,
        (double)publishes / elapsed,
        elapsed * 1000000.0 / (double)publishes, state.sink);
    return true;
}

int
main(void)
{
    krtsp_frame *private_ring = NULL;
    krtsp_frame *shared_ring = NULL;
    krtsp_frame *shared_reader = NULL;
    char name[64];
    bool ok = true;

    (void)printf("frame ring, %dx%d RGBA\n", FRAME_W, FRAME_H);

    if (!krtsp_frame_init(&private_ring, FRAME_W, FRAME_H)) {
        (void)fprintf(stderr, "cannot build the private ring\n");
        return 1;
    }
    ok = ok && run_contention("private_publish", private_ring, private_ring,
                              50000u, false);
    ok = ok && run_contention("private_full_frame", private_ring,
                              private_ring, 300u, true);
    krtsp_frame_free(private_ring);

    (void)snprintf(name, sizeof(name), "bench-%ld", (long)getpid());
    if (!krtsp_frame_init_shared(&shared_ring, name, FRAME_W, FRAME_H, 2)) {
        (void)fprintf(stderr, "cannot build the shared ring\n");
        return 1;
    }
    if (!krtsp_frame_attach(&shared_reader, name)) {
        (void)fprintf(stderr, "cannot attach to the shared ring\n");
        krtsp_frame_free(shared_ring);
        return 1;
    }
    ok = ok && run_contention("shared_publish", shared_ring, shared_reader,
                              50000u, false);
    ok = ok && run_contention("shared_full_frame", shared_ring,
                              shared_reader, 300u, true);
    krtsp_frame_free(shared_reader);
    krtsp_frame_free(shared_ring);

    return ok ? 0 : 1;
}
