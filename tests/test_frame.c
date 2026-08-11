#include "kilix_rtsp.h"

#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

/* A unique-enough name per run, so a crashed earlier run cannot make this
 * one fail and two runs in parallel do not collide. */
static void ring_name(char *out, size_t capacity, const char *tag)
{
    (void)snprintf(out, capacity, "test-%ld-%s", (long)getpid(), tag);
}

static bool
test_shared_ring_round_trip(void)
{
    char name[64];
    krtsp_frame *producer = NULL;
    krtsp_frame *reader = NULL;
    const uint8_t *pixels;
    uint64_t sequence = 0u;
    uint8_t *back;

    ring_name(name, sizeof(name), "rt");
    CHECK(krtsp_frame_init_shared(&producer, name, W, H, 2));
    CHECK(producer != NULL);
    CHECK(krtsp_frame_name(producer) != NULL);
    CHECK(strstr(krtsp_frame_name(producer), name) != NULL);

    /* A second handle onto the same object is a separate consumer. */
    CHECK(krtsp_frame_attach(&reader, name));
    CHECK(reader != NULL);
    CHECK(krtsp_frame_size(reader) == krtsp_frame_size(producer));
    CHECK(krtsp_frame_back(reader) == NULL);

    /* Nothing published yet: a reader must be told so, not handed zeros. */
    CHECK(krtsp_frame_borrow(reader, NULL, NULL) == NULL);

    back = krtsp_frame_back(producer);
    CHECK(back != NULL);
    fill(back, krtsp_frame_size(producer), 0x5au);
    krtsp_frame_publish(producer, NULL);

    pixels = krtsp_frame_borrow(reader, &sequence, NULL);
    CHECK(pixels != NULL);
    CHECK(sequence == 1u);
    CHECK(pixels[0] == 0x5au);
    CHECK(pixels[krtsp_frame_size(reader) - 1u] == 0x5au);
    krtsp_frame_release(reader);

    krtsp_frame_free(reader);
    krtsp_frame_free(producer);

    /* The producer owns the object, so attaching after it goes away fails
     * rather than handing back a stale mapping. */
    CHECK(!krtsp_frame_attach(&reader, name));
    return true;
}

static bool
test_live_producer_name_is_not_replaced(void)
{
    char name[64];
    krtsp_frame *producer = NULL;
    krtsp_frame *collision = NULL;
    krtsp_frame *reader = NULL;

    ring_name(name, sizeof(name), "live");
    CHECK(krtsp_frame_init_shared(&producer, name, W, H, 1));
    CHECK(!krtsp_frame_init_shared(&collision, name, W, H, 1));
    CHECK(collision == NULL);

    /* The failed second producer must not unlink or replace the first. */
    CHECK(krtsp_frame_attach(&reader, name));
    fill(krtsp_frame_back(producer), krtsp_frame_size(producer), 0x39u);
    krtsp_frame_publish(producer, NULL);
    CHECK(krtsp_frame_borrow(reader, NULL, NULL) != NULL);
    krtsp_frame_release(reader);

    krtsp_frame_free(reader);
    krtsp_frame_free(producer);
    return true;
}

static bool
test_shared_mode_is_exact_under_strict_umask(void)
{
    char name[64];
    krtsp_frame *producer = NULL;
    krtsp_frame *reader = NULL;
    struct stat info;
    mode_t previous;
    bool initialized;
    int fd;

    ring_name(name, sizeof(name), "mode");
    previous = umask(0777);
    initialized = krtsp_frame_init_shared(&producer, name, W, H, 1);
    (void)umask(previous);
    CHECK(initialized);

    fd = shm_open(krtsp_frame_name(producer), O_RDWR | O_CLOEXEC, 0);
    CHECK(fd >= 0);
    CHECK(fstat(fd, &info) == 0);
    CHECK((info.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) ==
          (S_IRUSR | S_IWUSR));
    CHECK(close(fd) == 0);
    CHECK(krtsp_frame_attach(&reader, name));

    krtsp_frame_free(reader);
    krtsp_frame_free(producer);
    return true;
}

/* Reader capacity counts active borrowers, even when several hold the same
 * newest slot.  Counting distinct pinned slots let this case over-commit. */
static bool
test_same_frame_borrows_obey_reader_limit(void)
{
    char name[64];
    krtsp_frame *producer = NULL;
    krtsp_frame *first = NULL;
    krtsp_frame *second = NULL;
    krtsp_frame *third = NULL;

    ring_name(name, sizeof(name), "same");
    CHECK(krtsp_frame_init_shared(&producer, name, W, H, 2));
    CHECK(krtsp_frame_attach(&first, name));
    CHECK(krtsp_frame_attach(&second, name));
    CHECK(krtsp_frame_attach(&third, name));
    fill(krtsp_frame_back(producer), krtsp_frame_size(producer), 0x61u);
    krtsp_frame_publish(producer, NULL);

    CHECK(krtsp_frame_borrow(first, NULL, NULL) != NULL);
    CHECK(krtsp_frame_borrow(second, NULL, NULL) != NULL);
    CHECK(krtsp_frame_borrow(third, NULL, NULL) == NULL);

    krtsp_frame_release(second);
    krtsp_frame_release(first);
    krtsp_frame_free(third);
    krtsp_frame_free(second);
    krtsp_frame_free(first);
    krtsp_frame_free(producer);
    return true;
}

static bool
test_killed_borrower_is_reclaimed(void)
{
    char name[64];
    int ready[2];
    krtsp_frame *producer = NULL;
    krtsp_frame *successor = NULL;
    pid_t child;
    int status = 0;
    char byte = '\0';

    ring_name(name, sizeof(name), "dead-reader");
    CHECK(krtsp_frame_init_shared(&producer, name, W, H, 1));
    fill(krtsp_frame_back(producer), krtsp_frame_size(producer), 0x72u);
    krtsp_frame_publish(producer, NULL);
    CHECK(pipe(ready) == 0);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        krtsp_frame *reader = NULL;
        const uint8_t *pixels = NULL;

        (void)close(ready[0]);
        if (krtsp_frame_attach(&reader, name)) {
            pixels = krtsp_frame_borrow(reader, NULL, NULL);
        }
        if (pixels != NULL && pixels[0] == 0x72u) {
            (void)write(ready[1], "x", 1u);
            /* Deliberately skip release/free: the kernel removes mappings,
             * while the shared lease survives for the producer to reap. */
            _exit(0);
        }
        _exit(1);
    }
    (void)close(ready[1]);
    CHECK(read(ready[0], &byte, 1u) == 1);
    (void)close(ready[0]);
    CHECK(byte == 'x');
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    CHECK(krtsp_frame_attach(&successor, name));
    CHECK(krtsp_frame_borrow(successor, NULL, NULL) != NULL);
    krtsp_frame_release(successor);
    krtsp_frame_free(successor);
    krtsp_frame_free(producer);
    return true;
}

static bool
test_dead_producer_orphan_is_replaced(void)
{
    char name[64];
    int ready[2];
    krtsp_frame *replacement = NULL;
    krtsp_frame *reader = NULL;
    pid_t child;
    int status = 0;
    char byte = '\0';

    ring_name(name, sizeof(name), "orphan");
    CHECK(pipe(ready) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        krtsp_frame *orphan = NULL;

        (void)close(ready[0]);
        if (krtsp_frame_init_shared(&orphan, name, W, H, 1)) {
            (void)write(ready[1], "x", 1u);
            _exit(0); /* simulate a crash: leave the name behind */
        }
        _exit(1);
    }
    (void)close(ready[1]);
    CHECK(read(ready[0], &byte, 1u) == 1);
    (void)close(ready[0]);
    CHECK(byte == 'x');
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* An orphan cannot produce another frame, so attach rejects it. */
    CHECK(!krtsp_frame_attach(&reader, name));
    CHECK(reader == NULL);
    CHECK(krtsp_frame_init_shared(&replacement, name, W, H, 1));
    krtsp_frame_free(replacement);
    return true;
}

static bool
test_owner_teardown_preserves_existing_reader(void)
{
    char name[64];
    krtsp_frame *producer = NULL;
    krtsp_frame *reader = NULL;
    krtsp_frame *late = NULL;
    const uint8_t *pixels;

    ring_name(name, sizeof(name), "teardown");
    CHECK(krtsp_frame_init_shared(&producer, name, W, H, 1));
    CHECK(krtsp_frame_attach(&reader, name));
    fill(krtsp_frame_back(producer), krtsp_frame_size(producer), 0x83u);
    krtsp_frame_publish(producer, NULL);
    pixels = krtsp_frame_borrow(reader, NULL, NULL);
    CHECK(pixels != NULL && pixels[0] == 0x83u);

    krtsp_frame_free(producer);
    CHECK(!krtsp_frame_attach(&late, name));
    CHECK(pixels[0] == 0x83u);
    krtsp_frame_release(reader);
    krtsp_frame_free(reader);
    return true;
}

static bool
test_truncated_shared_object_is_rejected(void)
{
    char leaf[64];
    char name[KRTSP_FRAME_NAME_MAX];
    krtsp_frame *reader = NULL;
    int fd;

    ring_name(leaf, sizeof(leaf), "truncated");
    CHECK(snprintf(name, sizeof(name), "%s%s", KRTSP_FRAME_NAME_PREFIX,
                   leaf) > 0);
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, 1) == 0);
    CHECK(close(fd) == 0);
    CHECK(!krtsp_frame_attach(&reader, leaf));
    CHECK(reader == NULL);
    CHECK(shm_unlink(name) == 0);
    return true;
}

/* The claim the whole change rests on: a different process reads frames
 * this one decoded, without decoding anything itself. */
static bool
test_shared_ring_crosses_a_process(void)
{
    char name[64];
    krtsp_frame *producer = NULL;
    uint8_t *back;
    pid_t child;
    int status = 0;

    ring_name(name, sizeof(name), "fork");
    CHECK(krtsp_frame_init_shared(&producer, name, W, H, 2));
    back = krtsp_frame_back(producer);
    CHECK(back != NULL);
    fill(back, krtsp_frame_size(producer), 0xa7u);
    krtsp_frame_publish(producer, NULL);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        /* Child: a genuinely separate address space.  Exit 0 only if the
         * bytes the parent wrote are visible here. */
        krtsp_frame *reader = NULL;
        int result = 1;

        if (krtsp_frame_attach(&reader, name)) {
            uint64_t sequence = 0u;
            const uint8_t *pixels = krtsp_frame_borrow(reader, &sequence, NULL);

            if (pixels != NULL && sequence == 1u && pixels[0] == 0xa7u &&
                pixels[krtsp_frame_size(reader) - 1u] == 0xa7u) {
                result = 0;
            }
            krtsp_frame_release(reader);
            krtsp_frame_free(reader);
        }
        _exit(result);
    }
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);

    krtsp_frame_free(producer);
    return true;
}

/* The ring is sized max_readers + 2.  Borrowing beyond that would leave
 * the producer nowhere to write, so it is refused instead. */
static bool
test_shared_ring_reserves_a_slot_for_the_producer(void)
{
    char name[64];
    krtsp_frame *producer = NULL;
    krtsp_frame *first = NULL;
    krtsp_frame *second = NULL;
    krtsp_frame *third = NULL;
    uint8_t *back;

    ring_name(name, sizeof(name), "res");
    CHECK(krtsp_frame_init_shared(&producer, name, W, H, 2));   /* 4 slots */
    CHECK(krtsp_frame_attach(&first, name));
    CHECK(krtsp_frame_attach(&second, name));
    CHECK(krtsp_frame_attach(&third, name));

    back = krtsp_frame_back(producer);
    fill(back, krtsp_frame_size(producer), 1u);
    krtsp_frame_publish(producer, NULL);
    CHECK(krtsp_frame_borrow(first, NULL, NULL) != NULL);

    back = krtsp_frame_back(producer);
    fill(back, krtsp_frame_size(producer), 2u);
    krtsp_frame_publish(producer, NULL);
    CHECK(krtsp_frame_borrow(second, NULL, NULL) != NULL);

    /* Two held plus the producer's own slot is the whole reservation. */
    CHECK(krtsp_frame_borrow(third, NULL, NULL) == NULL);

    /* The producer keeps working throughout, which is the point of the
     * reservation. */
    back = krtsp_frame_back(producer);
    CHECK(back != NULL);
    fill(back, krtsp_frame_size(producer), 3u);
    krtsp_frame_publish(producer, NULL);

    krtsp_frame_release(first);
    CHECK(krtsp_frame_borrow(third, NULL, NULL) != NULL);
    krtsp_frame_release(third);
    krtsp_frame_release(second);

    krtsp_frame_free(third);
    krtsp_frame_free(second);
    krtsp_frame_free(first);
    krtsp_frame_free(producer);
    return true;
}

/* A reader holding a borrow must keep seeing its own frame while the
 * producer publishes newer ones into other slots. */
static bool
test_shared_ring_borrow_survives_publishes(void)
{
    char name[64];
    krtsp_frame *producer = NULL;
    krtsp_frame *reader = NULL;
    const uint8_t *held;
    uint8_t *back;
    size_t size;
    int i;

    ring_name(name, sizeof(name), "hold");
    CHECK(krtsp_frame_init_shared(&producer, name, W, H, 2));
    CHECK(krtsp_frame_attach(&reader, name));
    size = krtsp_frame_size(producer);

    back = krtsp_frame_back(producer);
    fill(back, size, 0x11u);
    krtsp_frame_publish(producer, NULL);
    held = krtsp_frame_borrow(reader, NULL, NULL);
    CHECK(held != NULL && held[0] == 0x11u);

    for (i = 0; i < 4; i++) {
        back = krtsp_frame_back(producer);
        CHECK(back != NULL);
        fill(back, size, (uint8_t)(0x20 + i));
        krtsp_frame_publish(producer, NULL);
        CHECK(held[0] == 0x11u);  /* the borrowed frame is untouched */
    }
    krtsp_frame_release(reader);
    krtsp_frame_free(reader);
    krtsp_frame_free(producer);
    return true;
}

static bool
test_shared_ring_rejections(void)
{
    char name[64];
    krtsp_frame *frame = NULL;

    ring_name(name, sizeof(name), "rej");
    CHECK(!krtsp_frame_init_shared(NULL, name, W, H, 1));
    CHECK(!krtsp_frame_init_shared(&frame, NULL, W, H, 1));
    CHECK(!krtsp_frame_init_shared(&frame, "", W, H, 1));
    CHECK(!krtsp_frame_init_shared(&frame, name, 0, H, 1));
    CHECK(!krtsp_frame_init_shared(&frame, name, W, -1, 1));
    CHECK(!krtsp_frame_init_shared(&frame, name, W, H, INT_MAX));
    /* a leaf, not a path: these would escape the namespace */
    CHECK(!krtsp_frame_init_shared(&frame, "has/slash", W, H, 1));
    CHECK(!krtsp_frame_init_shared(&frame, "has space", W, H, 1));
    CHECK(frame == NULL);

    CHECK(!krtsp_frame_attach(NULL, name));
    CHECK(!krtsp_frame_attach(&frame, "definitely-not-created"));
    CHECK(frame == NULL);

    /* a private ring has no object name */
    CHECK(krtsp_frame_init(&frame, W, H));
    CHECK(krtsp_frame_name(frame) == NULL);
    krtsp_frame_free(frame);
    return true;
}

typedef bool (*test_function)(void);

typedef struct test_case {
    const char *name;
    test_function function;
} test_case;

/*
 * Who is attached, which is a different question from who is borrowing.
 *
 * A producer that runs a model only while something is watching asks this
 * one, and asking the borrow count instead would answer "nobody" almost
 * always - a borrow lasts microseconds.
 */
static bool test_readers_are_counted_by_attachment(void)
{
    char name[64];
    krtsp_frame *producer = NULL;
    krtsp_frame *first = NULL;
    krtsp_frame *second = NULL;
    krtsp_frame *private_ring = NULL;

    ring_name(name, sizeof(name), "rd");
    CHECK(krtsp_frame_init_shared(&producer, name, W, H, 3));
    /* The producer is not a reader of its own ring. */
    CHECK(krtsp_frame_readers(producer) == 0u);

    CHECK(krtsp_frame_attach(&first, name));
    CHECK(krtsp_frame_readers(producer) == 1u);
    /* Counted while nothing is borrowed, which is the whole point. */
    CHECK(krtsp_frame_borrow(first, NULL, NULL) == NULL);
    CHECK(krtsp_frame_readers(producer) == 1u);

    CHECK(krtsp_frame_attach(&second, name));
    CHECK(krtsp_frame_readers(producer) == 2u);
    /* A reader sees the same count as the producer. */
    CHECK(krtsp_frame_readers(first) == 2u);

    krtsp_frame_free(second);
    CHECK(krtsp_frame_readers(producer) == 1u);
    krtsp_frame_free(first);
    CHECK(krtsp_frame_readers(producer) == 0u);

    krtsp_frame_free(producer);

    /* A private ring has no other process by construction. */
    CHECK(krtsp_frame_init(&private_ring, W, H));
    CHECK(krtsp_frame_readers(private_ring) == 0u);
    krtsp_frame_free(private_ring);
    CHECK(krtsp_frame_readers(NULL) == 0u);
    return true;
}

int
main(void)
{
    static const test_case tests[] = {
        {"lifecycle", test_lifecycle},
        {"newest wins", test_newest_wins},
        {"publish during borrow", test_publish_during_borrow},
        {"age reports staleness", test_age_reports_staleness},
        {"concurrent publish and borrow", test_concurrent_publish_and_borrow},
        {"rejections", test_rejections},
        {"shared ring round trip", test_shared_ring_round_trip},
        {"live producer name is not replaced",
         test_live_producer_name_is_not_replaced},
        {"shared mode is exact under strict umask",
         test_shared_mode_is_exact_under_strict_umask},
        {"same frame borrows obey reader limit",
         test_same_frame_borrows_obey_reader_limit},
        {"killed borrower is reclaimed", test_killed_borrower_is_reclaimed},
        {"dead producer orphan is replaced",
         test_dead_producer_orphan_is_replaced},
        {"owner teardown preserves existing reader",
         test_owner_teardown_preserves_existing_reader},
        {"truncated shared object is rejected",
         test_truncated_shared_object_is_rejected},
        {"shared ring crosses a process",
         test_shared_ring_crosses_a_process},
        {"shared ring reserves a slot for the producer",
         test_shared_ring_reserves_a_slot_for_the_producer},
        {"shared ring borrow survives publishes",
         test_shared_ring_borrow_survives_publishes},
        {"shared ring rejections", test_shared_ring_rejections},
        {"readers are counted by attachment",
         test_readers_are_counted_by_attachment}
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
