/*
 * Single-slot, newest-wins frame handoff, built on three buffers.
 *
 * Three, not two.  A two-buffer design where publish and release both
 * swap front and back has a real data race, and it is not a subtle one
 * once seen: the producer holds the `back` pointer across the lock while
 * it fills the frame, and a consumer-side swap rewrites that pointer
 * under it.  ThreadSanitizer catches it immediately - consumer writing
 * frame->back in release(), producer reading frame->back - and no amount
 * of care in the producer fixes it, because the producer cannot hold the
 * lock while it does a megabyte of pipe I/O.
 *
 * With three buffers each side owns one outright:
 *
 *   back    the producer's, always.  Never read or written by anyone else.
 *   ready   the newest published frame, or empty.  The only shared slot,
 *           and it is only ever exchanged under the lock.
 *   front   the consumer's while borrowed.  Never touched by the producer.
 *
 * publish() exchanges back with ready; borrow() exchanges front with
 * ready.  Neither side's working buffer is ever aliased by the other, so
 * the mutex protects two pointer swaps and some counters and nothing
 * else.  It is never held across a read() from the camera or a write()
 * to the terminal: doing so would couple them, and a slow terminal would
 * stall capture - the exact failure newest-wins exists to prevent.
 *
 * Because publish() exchanges the producer's buffer, the producer must
 * call krtsp_frame_back() again after every publish.  That is documented
 * on the function and is the one rule a caller has to follow.
 */

#include "kilix_rtsp.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct krtsp_frame {
    pthread_mutex_t lock;
    uint8_t *back;    /* producer-owned */
    uint8_t *ready;   /* shared handoff slot */
    uint8_t *front;   /* consumer-owned */
    size_t size;
    int width;
    int height;

    uint64_t sequence;         /* increments once per published frame */
    uint64_t ready_sequence;   /* sequence of the frame sitting in ready */
    uint64_t front_sequence;   /* sequence of the frame in front */
    struct timespec ready_written;
    struct timespec front_written;
    bool has_ready;
    bool has_front;
    bool borrowed;

    uint64_t published;
    uint64_t dropped;
};

static int64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
}

static int age_of(const struct timespec *written)
{
    int64_t stamp = (int64_t)written->tv_sec * 1000 +
                    (int64_t)written->tv_nsec / 1000000;
    int64_t age = monotonic_ms() - stamp;

    if (age < 0) {
        age = 0;
    }
    return age > (int64_t)INT32_MAX ? INT32_MAX : (int)age;
}

bool krtsp_frame_init(krtsp_frame **out, int width, int height)
{
    krtsp_frame *frame;
    size_t size;

    if (out == NULL) {
        return false;
    }
    *out = NULL;
    if (width <= 0 || height <= 0) {
        return false;
    }
    if ((size_t)width > SIZE_MAX / 4u / (size_t)height) {
        return false;
    }
    size = (size_t)width * (size_t)height * 4u;

    frame = calloc(1u, sizeof(*frame));
    if (frame == NULL) {
        return false;
    }
    frame->back = calloc(1u, size);
    frame->ready = calloc(1u, size);
    frame->front = calloc(1u, size);
    if (frame->back == NULL || frame->ready == NULL || frame->front == NULL) {
        free(frame->back);
        free(frame->ready);
        free(frame->front);
        free(frame);
        return false;
    }
    if (pthread_mutex_init(&frame->lock, NULL) != 0) {
        free(frame->back);
        free(frame->ready);
        free(frame->front);
        free(frame);
        return false;
    }
    frame->size = size;
    frame->width = width;
    frame->height = height;
    *out = frame;
    return true;
}

void krtsp_frame_free(krtsp_frame *frame)
{
    if (frame == NULL) {
        return;
    }
    (void)pthread_mutex_destroy(&frame->lock);
    free(frame->back);
    free(frame->ready);
    free(frame->front);
    free(frame);
}

uint8_t *krtsp_frame_back(krtsp_frame *frame)
{
    uint8_t *buffer;

    if (frame == NULL) {
        return NULL;
    }
    /* Read under the lock: publish() exchanges this pointer, and an
     * unsynchronized read of it is precisely the race three buffers
     * exist to remove. */
    pthread_mutex_lock(&frame->lock);
    buffer = frame->back;
    pthread_mutex_unlock(&frame->lock);
    return buffer;
}

size_t krtsp_frame_size(const krtsp_frame *frame)
{
    return frame != NULL ? frame->size : 0u;
}

void krtsp_frame_publish(krtsp_frame *frame, bool *dropped)
{
    bool replaced = false;
    uint8_t *swap;

    if (frame == NULL) {
        if (dropped != NULL) {
            *dropped = false;
        }
        return;
    }
    pthread_mutex_lock(&frame->lock);

    /* A frame already waiting in ready is about to be replaced by a
     * newer one.  Count it before the exchange, so the statistic means
     * "frames the consumer never saw". */
    if (frame->has_ready) {
        replaced = true;
        frame->dropped++;
    }

    swap = frame->ready;
    frame->ready = frame->back;
    frame->back = swap;

    frame->sequence++;
    frame->published++;
    frame->ready_sequence = frame->sequence;
    (void)clock_gettime(CLOCK_MONOTONIC, &frame->ready_written);
    frame->has_ready = true;

    pthread_mutex_unlock(&frame->lock);
    if (dropped != NULL) {
        *dropped = replaced;
    }
}

const uint8_t *krtsp_frame_borrow(
    krtsp_frame *frame, uint64_t *sequence, int *age_ms)
{
    const uint8_t *pixels;

    if (frame == NULL) {
        return NULL;
    }
    pthread_mutex_lock(&frame->lock);
    if (frame->borrowed) {
        pthread_mutex_unlock(&frame->lock);
        return NULL;
    }
    if (frame->has_ready) {
        uint8_t *swap = frame->front;

        frame->front = frame->ready;
        frame->ready = swap;
        frame->has_ready = false;
        frame->has_front = true;
        frame->front_sequence = frame->ready_sequence;
        frame->front_written = frame->ready_written;
    }
    if (!frame->has_front) {
        /* Nothing has ever been published: say so rather than handing
         * back a zeroed buffer the caller would render as black. */
        pthread_mutex_unlock(&frame->lock);
        return NULL;
    }
    frame->borrowed = true;
    pixels = frame->front;
    if (sequence != NULL) {
        *sequence = frame->front_sequence;
    }
    if (age_ms != NULL) {
        *age_ms = age_of(&frame->front_written);
    }
    pthread_mutex_unlock(&frame->lock);
    return pixels;
}

void krtsp_frame_release(krtsp_frame *frame)
{
    if (frame == NULL) {
        return;
    }
    /* Nothing to exchange: front stays the consumer's until the next
     * borrow takes a newer frame from ready. */
    pthread_mutex_lock(&frame->lock);
    frame->borrowed = false;
    pthread_mutex_unlock(&frame->lock);
}

void krtsp_frame_stats(
    const krtsp_frame *frame, uint64_t *published, uint64_t *dropped)
{
    krtsp_frame *mutable_frame;

    if (frame == NULL) {
        return;
    }
    /* Locking is logically const: it protects the snapshot without
     * changing observable state. */
    mutable_frame = (krtsp_frame *)frame;
    pthread_mutex_lock(&mutable_frame->lock);
    if (published != NULL) {
        *published = mutable_frame->published;
    }
    if (dropped != NULL) {
        *dropped = mutable_frame->dropped;
    }
    pthread_mutex_unlock(&mutable_frame->lock);
}
