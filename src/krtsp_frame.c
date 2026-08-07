/*
 * Newest-wins frame handoff, in one process or across several.
 *
 * The policy is unchanged from the original three-buffer version: a new
 * frame replaces an unread one rather than queueing behind it, because
 * video is only useful live and a queue trades the one thing that matters
 * (latency) for the one that does not (completeness).
 *
 * What changed is that the buffers are now a ring of slots addressed by
 * index, and the ring can live in a POSIX shared-memory object so
 * separate processes read the same decode.  Pointer swapping - what the
 * three-buffer version did - cannot cross a process boundary at all: the
 * same object maps to different addresses in different processes, so a
 * pointer stored by the producer is meaningless to a reader.  Indices are
 * the same number everywhere.
 *
 * Three buffers were the minimum for one reader, and the reasoning still
 * holds: two race, because a consumer-side swap rewrites the pointer the
 * producer is filling through.  Generalised, the ring needs
 *
 *     1 slot the producer is filling
 *   + 1 slot holding the newest published frame
 *   + 1 slot per reader currently holding a borrow
 *
 * so slots = max_readers + 2, and the private case is that with one
 * reader: three, as before.  borrow() refuses rather than over-committing
 * when pinning would leave the producer nowhere to write, which is what
 * makes "the producer always has a slot" an invariant instead of a hope.
 *
 * Only the small header - indices, counters, pin counts - is touched
 * under the lock.  It is never held across a read() from the camera or a
 * write() to the terminal: doing so would couple them, and a slow reader
 * would stall capture, the exact failure newest-wins exists to prevent.
 *
 * Because publish() moves the producer to a different slot, the producer
 * must call krtsp_frame_back() again after every publish.  That is the
 * one rule a caller has to follow, and it is unchanged.
 */

#include "kilix_rtsp.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RING_MAGIC 0x4b525450u   /* 'KRTP' */
#define RING_VERSION 1u
#define RING_MAX_SLOTS 32
#define RING_ALIGN 64u

/*
 * The shared header.  Everything here is index and counter arithmetic:
 * nothing in it is a pointer, because a pointer written by one process
 * means nothing in another.  Timestamps are milliseconds rather than
 * struct timespec so the layout does not depend on that struct's padding.
 */
struct ring_header {
    uint32_t magic;
    uint32_t version;
    int32_t width;
    int32_t height;
    uint64_t frame_size;
    int32_t slots;

    int32_t writing;            /* slot the producer owns */
    int32_t newest;             /* newest published slot, -1 when none */
    int32_t newest_taken;       /* has anyone borrowed the newest frame */
    uint64_t sequence;
    uint64_t newest_sequence;
    int64_t newest_written_ms;
    uint64_t published;
    uint64_t dropped;
    int32_t pin[RING_MAX_SLOTS];

    pthread_mutex_t lock;
};

struct krtsp_frame {
    struct ring_header *header;
    uint8_t *slots;             /* slots[i] at slots + i * frame_size */
    size_t map_size;
    bool shared;
    bool owns;                  /* created the object, so unlinks it */
    int held;                   /* slot this handle has pinned, -1 = none */
    char name[KRTSP_FRAME_NAME_MAX];
};

static int64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
}

static size_t align_up(size_t value)
{
    return (value + (RING_ALIGN - 1u)) & ~(size_t)(RING_ALIGN - 1u);
}

/*
 * Lock, recovering from a holder that died mid-update.
 *
 * A shared ring outlives any one process, so the producer can be killed
 * between two index writes.  A robust mutex reports that as EOWNERDEAD
 * instead of deadlocking every reader forever.  The critical sections
 * here are a handful of integer stores, so the worst inconsistency a dead
 * holder can leave is a stale index, and marking the lock consistent and
 * continuing is safe.  A plain mutex would strand the whole camera.
 */
static bool ring_lock(struct ring_header *header)
{
    int rc = pthread_mutex_lock(&header->lock);

    if (rc == 0) {
        return true;
    }
#ifdef EOWNERDEAD
    if (rc == EOWNERDEAD) {
        (void)pthread_mutex_consistent(&header->lock);
        return true;
    }
#endif
    return false;
}

static void ring_unlock(struct ring_header *header)
{
    (void)pthread_mutex_unlock(&header->lock);
}

/* Slots the producer may claim: unpinned, and not the newest frame. */
static int pick_writable(const struct ring_header *header, int avoid)
{
    for (int i = 0; i < header->slots; i++) {
        if (i != avoid && header->pin[i] == 0) {
            return i;
        }
    }
    return -1;
}

static int pinned_count(const struct ring_header *header)
{
    int total = 0;

    for (int i = 0; i < header->slots; i++) {
        total += header->pin[i] != 0 ? 1 : 0;
    }
    return total;
}

static bool header_init(struct ring_header *header, int width, int height,
                        size_t frame_size, int slots, bool shared)
{
    pthread_mutexattr_t attributes;

    (void)memset(header, 0, sizeof(*header));
    if (pthread_mutexattr_init(&attributes) != 0) {
        return false;
    }
    if (shared) {
        /* Without PROCESS_SHARED the mutex is only meaningful to the
         * process that created it, and every other mapping would be
         * locking a private copy of nothing. */
        if (pthread_mutexattr_setpshared(&attributes,
                                         PTHREAD_PROCESS_SHARED) != 0) {
            (void)pthread_mutexattr_destroy(&attributes);
            return false;
        }
#ifdef PTHREAD_MUTEX_ROBUST
        (void)pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST);
#endif
    }
    if (pthread_mutex_init(&header->lock, &attributes) != 0) {
        (void)pthread_mutexattr_destroy(&attributes);
        return false;
    }
    (void)pthread_mutexattr_destroy(&attributes);

    header->width = width;
    header->height = height;
    header->frame_size = (uint64_t)frame_size;
    header->slots = slots;
    header->writing = 0;
    header->newest = -1;
    header->newest_taken = 1;
    header->version = RING_VERSION;
    /* Magic last: a reader that maps the object mid-construction sees
     * zero here and refuses, rather than reading half-built indices. */
    header->magic = RING_MAGIC;
    return true;
}

static bool geometry_ok(int width, int height, int slots, size_t *frame_size,
                        size_t *map_size)
{
    size_t size;

    if (width <= 0 || height <= 0) {
        return false;
    }
    if (slots < 3 || slots > RING_MAX_SLOTS) {
        return false;
    }
    if ((size_t)width > SIZE_MAX / 4u / (size_t)height) {
        return false;
    }
    size = (size_t)width * (size_t)height * 4u;
    if (size > (SIZE_MAX - align_up(sizeof(struct ring_header))) /
                   (size_t)slots) {
        return false;
    }
    *frame_size = size;
    *map_size = align_up(sizeof(struct ring_header)) + size * (size_t)slots;
    return true;
}

bool krtsp_frame_init(krtsp_frame **out, int width, int height)
{
    krtsp_frame *frame;
    size_t frame_size, map_size;
    uint8_t *block;

    if (out == NULL) {
        return false;
    }
    *out = NULL;
    if (!geometry_ok(width, height, 3, &frame_size, &map_size)) {
        return false;
    }
    frame = calloc(1u, sizeof(*frame));
    if (frame == NULL) {
        return false;
    }
    block = calloc(1u, map_size);
    if (block == NULL) {
        free(frame);
        return false;
    }
    frame->header = (struct ring_header *)(void *)block;
    if (!header_init(frame->header, width, height, frame_size, 3, false)) {
        free(block);
        free(frame);
        return false;
    }
    frame->slots = block + align_up(sizeof(struct ring_header));
    frame->map_size = map_size;
    frame->shared = false;
    frame->owns = true;
    frame->held = -1;
    *out = frame;
    return true;
}

/* POSIX shm names are a flat namespace, so the prefix keeps ours apart
 * from every other program's and makes orphans identifiable. */
static bool shm_name_for(const char *leaf, char *out, size_t capacity)
{
    int printed;

    if (leaf == NULL || leaf[0] == '\0') {
        return false;
    }
    for (const char *p = leaf; *p != '\0'; p++) {
        /* '/' would create a second path component, which POSIX does not
         * define; the rest keeps names shell- and log-safe. */
        if (*p == '/' || *p == '\\' || (unsigned char)*p <= ' ') {
            return false;
        }
    }
    printed = snprintf(out, capacity, "%s%s", KRTSP_FRAME_NAME_PREFIX, leaf);
    return printed > 0 && (size_t)printed < capacity;
}

bool krtsp_frame_init_shared(krtsp_frame **out, const char *name, int width,
                             int height, int max_readers)
{
    krtsp_frame *frame;
    size_t frame_size, map_size;
    void *mapping;
    int slots;
    int fd;

    if (out == NULL) {
        return false;
    }
    *out = NULL;
    if (max_readers < 1) {
        max_readers = 1;
    }
    slots = max_readers + 2;
    if (!geometry_ok(width, height, slots, &frame_size, &map_size)) {
        return false;
    }
    frame = calloc(1u, sizeof(*frame));
    if (frame == NULL) {
        return false;
    }
    if (!shm_name_for(name, frame->name, sizeof(frame->name))) {
        free(frame);
        return false;
    }

    fd = shm_open(frame->name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 && errno == EEXIST) {
        /* One producer owns a given camera name, so an existing object is
         * a leak from a dead run rather than a live peer.  Unlink and
         * recreate: anyone still holding the old object keeps their
         * mapping and simply stops seeing new frames, which beats
         * refusing to start. */
        (void)shm_unlink(frame->name);
        fd = shm_open(frame->name, O_RDWR | O_CREAT | O_EXCL, 0600);
    }
    if (fd < 0) {
        free(frame);
        return false;
    }
    if (ftruncate(fd, (off_t)map_size) != 0) {
        (void)close(fd);
        (void)shm_unlink(frame->name);
        free(frame);
        return false;
    }
    mapping = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    /* The descriptor is not needed once mapped, and holding it would leak
     * one per camera. */
    (void)close(fd);
    if (mapping == MAP_FAILED) {
        (void)shm_unlink(frame->name);
        free(frame);
        return false;
    }
    frame->header = (struct ring_header *)mapping;
    if (!header_init(frame->header, width, height, frame_size, slots, true)) {
        (void)munmap(mapping, map_size);
        (void)shm_unlink(frame->name);
        free(frame);
        return false;
    }
    frame->slots = (uint8_t *)mapping + align_up(sizeof(struct ring_header));
    frame->map_size = map_size;
    frame->shared = true;
    frame->owns = true;
    frame->held = -1;
    *out = frame;
    return true;
}

bool krtsp_frame_attach(krtsp_frame **out, const char *name)
{
    krtsp_frame *frame;
    struct ring_header probe;
    void *mapping;
    size_t map_size;
    ssize_t got;
    int fd;

    if (out == NULL) {
        return false;
    }
    *out = NULL;
    frame = calloc(1u, sizeof(*frame));
    if (frame == NULL) {
        return false;
    }
    if (!shm_name_for(name, frame->name, sizeof(frame->name))) {
        free(frame);
        return false;
    }
    fd = shm_open(frame->name, O_RDWR, 0600);
    if (fd < 0) {
        free(frame);
        return false;
    }
    /* Read the header before mapping: the full size is computed from it,
     * and mapping a bogus object at a bogus size is how a reader turns a
     * missing producer into a crash. */
    got = read(fd, &probe, sizeof(probe));
    if (got != (ssize_t)sizeof(probe) || probe.magic != RING_MAGIC ||
        probe.version != RING_VERSION || probe.slots < 3 ||
        probe.slots > RING_MAX_SLOTS || probe.frame_size == 0u ||
        probe.frame_size > SIZE_MAX / (size_t)probe.slots) {
        (void)close(fd);
        free(frame);
        return false;
    }
    map_size = align_up(sizeof(struct ring_header)) +
               (size_t)probe.frame_size * (size_t)probe.slots;
    mapping = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    (void)close(fd);
    if (mapping == MAP_FAILED) {
        free(frame);
        return false;
    }
    frame->header = (struct ring_header *)mapping;
    frame->slots = (uint8_t *)mapping + align_up(sizeof(struct ring_header));
    frame->map_size = map_size;
    frame->shared = true;
    frame->owns = false;   /* attaching never unlinks the producer's object */
    frame->held = -1;
    *out = frame;
    return true;
}

const char *krtsp_frame_name(const krtsp_frame *frame)
{
    if (frame == NULL || !frame->shared) {
        return NULL;
    }
    return frame->name;
}

void krtsp_frame_free(krtsp_frame *frame)
{
    if (frame == NULL) {
        return;
    }
    /* Drop a pin first, or a reader that exits mid-borrow permanently
     * costs the producer a slot. */
    if (frame->held >= 0 && ring_lock(frame->header)) {
        if (frame->header->pin[frame->held] > 0) {
            frame->header->pin[frame->held]--;
        }
        ring_unlock(frame->header);
    }
    if (frame->shared) {
        if (frame->owns) {
            (void)pthread_mutex_destroy(&frame->header->lock);
            (void)shm_unlink(frame->name);
        }
        (void)munmap(frame->header, frame->map_size);
    } else {
        (void)pthread_mutex_destroy(&frame->header->lock);
        free(frame->header);
    }
    free(frame);
}

uint8_t *krtsp_frame_back(krtsp_frame *frame)
{
    uint8_t *buffer;

    if (frame == NULL) {
        return NULL;
    }
    /* Read the index under the lock: publish() moves it, and an
     * unsynchronised read is precisely the race the slot design removes. */
    if (!ring_lock(frame->header)) {
        return NULL;
    }
    buffer = frame->slots +
             (size_t)frame->header->writing * (size_t)frame->header->frame_size;
    ring_unlock(frame->header);
    return buffer;
}

size_t krtsp_frame_size(const krtsp_frame *frame)
{
    return frame != NULL ? (size_t)frame->header->frame_size : 0u;
}

void krtsp_frame_publish(krtsp_frame *frame, bool *dropped)
{
    bool replaced = false;

    if (dropped != NULL) {
        *dropped = false;
    }
    if (frame == NULL || !ring_lock(frame->header)) {
        return;
    }
    {
        struct ring_header *header = frame->header;
        int next;

        /* A frame nobody borrowed is about to be replaced by a newer one.
         * Count it before the exchange, so the statistic keeps meaning
         * "frames no consumer ever saw". */
        if (header->newest >= 0 && !header->newest_taken) {
            replaced = true;
            header->dropped++;
        }

        next = pick_writable(header, header->writing);
        if (next < 0) {
            /* Cannot happen while borrow() honours the reservation below,
             * and dropping the frame is the safe response if it ever
             * does: overwriting a pinned slot would corrupt a reader. */
            ring_unlock(header);
            return;
        }
        header->newest = header->writing;
        header->writing = next;

        header->sequence++;
        header->published++;
        header->newest_sequence = header->sequence;
        header->newest_written_ms = monotonic_ms();
        header->newest_taken = 0;
    }
    ring_unlock(frame->header);
    if (dropped != NULL) {
        *dropped = replaced;
    }
}

const uint8_t *krtsp_frame_borrow(
    krtsp_frame *frame, uint64_t *sequence, int *age_ms)
{
    const uint8_t *pixels;
    struct ring_header *header;

    if (frame == NULL) {
        return NULL;
    }
    header = frame->header;
    if (!ring_lock(header)) {
        return NULL;
    }
    if (frame->held >= 0 || header->newest < 0) {
        /* Already holding one, or nothing has ever been published: say so
         * rather than handing back a zeroed buffer a caller would render
         * as black. */
        ring_unlock(header);
        return NULL;
    }
    /* Reserve a slot for the producer and one for the next publish.
     * Refusing here is what makes "the producer always has somewhere to
     * write" true by construction rather than by luck. */
    if (pinned_count(header) >= header->slots - 2) {
        ring_unlock(header);
        return NULL;
    }
    frame->held = header->newest;
    header->pin[frame->held]++;
    header->newest_taken = 1;

    pixels = frame->slots +
             (size_t)frame->held * (size_t)header->frame_size;
    if (sequence != NULL) {
        *sequence = header->newest_sequence;
    }
    if (age_ms != NULL) {
        int64_t age = monotonic_ms() - header->newest_written_ms;

        if (age < 0) {
            age = 0;
        }
        *age_ms = age > (int64_t)INT32_MAX ? INT32_MAX : (int)age;
    }
    ring_unlock(header);
    return pixels;
}

void krtsp_frame_release(krtsp_frame *frame)
{
    if (frame == NULL || frame->held < 0) {
        return;
    }
    if (!ring_lock(frame->header)) {
        return;
    }
    if (frame->header->pin[frame->held] > 0) {
        frame->header->pin[frame->held]--;
    }
    frame->held = -1;
    ring_unlock(frame->header);
}

void krtsp_frame_stats(
    const krtsp_frame *frame, uint64_t *published, uint64_t *dropped)
{
    struct ring_header *header;

    if (frame == NULL) {
        return;
    }
    /* Locking is logically const: it protects the snapshot without
     * changing observable state. */
    header = ((krtsp_frame *)frame)->header;
    if (!ring_lock(header)) {
        return;
    }
    if (published != NULL) {
        *published = header->published;
    }
    if (dropped != NULL) {
        *dropped = header->dropped;
    }
    ring_unlock(header);
}
