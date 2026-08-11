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
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RING_MAGIC 0x4b525450u   /* 'KRTP' */
#define RING_VERSION 3u
#define RING_MAX_SLOTS 32
#define RING_MAX_READERS (RING_MAX_SLOTS - 2)
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
    int64_t owner_pid;

    int32_t writing;            /* slot the producer owns */
    int32_t newest;             /* newest published slot, -1 when none */
    int32_t newest_taken;       /* has anyone borrowed the newest frame */
    uint64_t sequence;
    uint64_t newest_sequence;
    int64_t newest_written_ms;
    uint64_t published;
    uint64_t dropped;
    int32_t pin[RING_MAX_SLOTS];
    int64_t borrower_pid[RING_MAX_READERS];
    int32_t borrower_slot[RING_MAX_READERS];
    /*
     * Who is attached, as opposed to who is mid-borrow.
     *
     * A borrow lasts microseconds, so counting borrows answers "is anyone
     * reading right this instant" - which is almost always no, even with
     * a viewer on screen.  A producer that wants to know whether anything
     * is watching, so it can decide whether to run a model at all, needs
     * attachment instead, and that is this.  Reclaimed by liveness like
     * the borrow leases, since a reader killed with -9 unmaps nothing.
     */
    int64_t reader_pid[RING_MAX_READERS];

    pthread_mutex_t lock;
};

struct krtsp_frame {
    struct ring_header *header;
    int reader;                 /* shared reader entry, -1 = none */
    uint8_t *slots;             /* slots[i] at slots + i * frame_size */
    size_t map_size;
    bool shared;
    bool owns;                  /* created the object, so unlinks it */
    int held;                   /* slot this handle has pinned, -1 = none */
    int lease;                  /* shared borrower entry, -1 = none */
    int owner_fd;               /* holds the producer's lifetime lock */
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
static bool pid_is_alive(int64_t value)
{
    pid_t pid;

    if (value <= 0 || value > INT_MAX) {
        return false;
    }
    pid = (pid_t)value;
    return kill(pid, 0) == 0 || errno != ESRCH;
}

/* Rebuild all derived state after a process died in a critical section.  A
 * borrower lease is authoritative; pin[] is merely its fast per-slot index. */
static bool repair_header(struct ring_header *header)
{
    int max_readers;

    if (header->slots < 3 || header->slots > RING_MAX_SLOTS) {
        return false;
    }
    max_readers = header->slots - 2;
    (void)memset(header->pin, 0, sizeof(header->pin));
    for (int index = 0; index < RING_MAX_READERS; ++index) {
        int slot = header->borrower_slot[index];

        if (index >= max_readers || !pid_is_alive(header->borrower_pid[index]) ||
            slot < 0 || slot >= header->slots) {
            header->borrower_pid[index] = 0;
            header->borrower_slot[index] = -1;
            continue;
        }
        header->pin[slot]++;
    }
    if (header->writing < 0 || header->writing >= header->slots) {
        header->writing = 0;
    }
    if (header->newest < -1 || header->newest >= header->slots ||
        header->newest == header->writing) {
        header->newest = -1;
        header->newest_taken = 1;
    }
    return true;
}

static bool ring_lock(struct ring_header *header)
{
    int rc = pthread_mutex_lock(&header->lock);

    if (rc == 0) {
        return true;
    }
#ifdef EOWNERDEAD
    if (rc == EOWNERDEAD) {
        if (repair_header(header) &&
            pthread_mutex_consistent(&header->lock) == 0) {
            return true;
        }
        (void)pthread_mutex_unlock(&header->lock);
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

static int active_borrows(const struct ring_header *header)
{
    int total = 0;
    int max_readers = header->slots - 2;

    for (int i = 0; i < max_readers; i++) {
        total += header->borrower_pid[i] > 0 ? 1 : 0;
    }
    return total;
}

static void reclaim_dead_borrows(struct ring_header *header)
{
    int max_readers = header->slots - 2;

    for (int index = 0; index < max_readers; ++index) {
        int slot = header->borrower_slot[index];

        if (header->borrower_pid[index] <= 0 ||
            pid_is_alive(header->borrower_pid[index])) {
            continue;
        }
        header->borrower_pid[index] = 0;
        header->borrower_slot[index] = -1;
        if (slot >= 0 && slot < header->slots && header->pin[slot] > 0) {
            header->pin[slot]--;
        }
    }
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
#ifdef __linux__
        if (pthread_mutexattr_setrobust(&attributes,
                                        PTHREAD_MUTEX_ROBUST) != 0) {
            (void)pthread_mutexattr_destroy(&attributes);
            return false;
        }
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
    header->owner_pid = (int64_t)getpid();
    header->writing = 0;
    header->newest = -1;
    header->newest_taken = 1;
    for (int index = 0; index < RING_MAX_READERS; ++index) {
        header->borrower_slot[index] = -1;
    }
    header->version = RING_VERSION;
    /* Magic last: a reader that maps the object mid-construction sees
     * zero here and refuses, rather than reading half-built indices. */
    __atomic_store_n(&header->magic, RING_MAGIC, __ATOMIC_RELEASE);
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
    frame->lease = -1;
    frame->owner_fd = -1;
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

/*
 * Create the named object while holding an advisory lifetime lock.
 *
 * O_EXCL alone distinguishes "some object exists" from "none exists" but
 * cannot distinguish a live producer from a crash orphan.  Unconditionally
 * unlinking EEXIST split live readers across two objects.  A producer keeps
 * this flock for its lifetime; a replacement may unlink only an unlocked
 * orphan, and flock is released by the kernel on a crash.
 */
static int create_locked_shm(const char *name)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

        if (fd >= 0) {
            /* shm_open creation modes are still filtered through umask.
             * Enforce the public exact-0600 contract on the descriptor we
             * just created, including under an unusually strict umask. */
            if (flock(fd, LOCK_EX | LOCK_NB) == 0 &&
                fchmod(fd, S_IRUSR | S_IWUSR) == 0) {
                return fd;
            }
            {
                int saved = errno;

                (void)shm_unlink(name);
                (void)close(fd);
                errno = saved;
                return -1;
            }
        }
        if (errno != EEXIST) {
            return -1;
        }

        fd = shm_open(name, O_RDWR | O_CLOEXEC, 0600);
        if (fd < 0) {
            if (errno == ENOENT) {
                continue;
            }
            return -1;
        }
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            int saved = errno;

            (void)close(fd);
            errno = saved == EWOULDBLOCK ? EEXIST : saved;
            return -1;
        }
        /* The lock proves no producer still owns this object.  Keep it
         * until the replacement exists and owns its own lock, so two crash
         * recoveries cannot unlink one another. */
        if (shm_unlink(name) != 0 && errno != ENOENT) {
            int saved = errno;

            (void)close(fd);
            errno = saved;
            return -1;
        }
        {
            int replacement = shm_open(
                name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

            if (replacement >= 0 &&
                flock(replacement, LOCK_EX | LOCK_NB) == 0 &&
                fchmod(replacement, S_IRUSR | S_IWUSR) == 0) {
                (void)close(fd);
                return replacement;
            }
            {
                int saved = errno;

                if (replacement >= 0) {
                    (void)shm_unlink(name);
                    (void)close(replacement);
                }
                (void)close(fd);
                if (saved == EEXIST || saved == ENOENT) {
                    continue;
                }
                errno = saved;
                return -1;
            }
        }
    }
    errno = EAGAIN;
    return -1;
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
    if (max_readers > RING_MAX_READERS) {
        return false;
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

    fd = create_locked_shm(frame->name);
    if (fd < 0) {
        free(frame);
        return false;
    }
    if ((off_t)map_size < 0 || (size_t)(off_t)map_size != map_size ||
        ftruncate(fd, (off_t)map_size) != 0) {
        (void)shm_unlink(frame->name);
        (void)close(fd);
        free(frame);
        return false;
    }
    mapping = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        (void)shm_unlink(frame->name);
        (void)close(fd);
        free(frame);
        return false;
    }
    frame->header = (struct ring_header *)mapping;
    if (!header_init(frame->header, width, height, frame_size, slots, true)) {
        (void)munmap(mapping, map_size);
        (void)shm_unlink(frame->name);
        (void)close(fd);
        free(frame);
        return false;
    }
    frame->slots = (uint8_t *)mapping + align_up(sizeof(struct ring_header));
    frame->map_size = map_size;
    frame->shared = true;
    frame->owns = true;
    frame->held = -1;
    frame->lease = -1;
    frame->owner_fd = fd;
    *out = frame;
    return true;
}

static bool pread_full(int fd, void *buffer, size_t size)
{
    size_t total = 0u;

    while (total < size) {
        ssize_t got = pread(fd, (uint8_t *)buffer + total, size - total,
                            (off_t)total);

        if (got > 0) {
            total += (size_t)got;
        } else if (got < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool immutable_header_ok(const struct ring_header *header,
                                size_t *map_size)
{
    size_t expected_frame;

    if (__atomic_load_n(&header->magic, __ATOMIC_ACQUIRE) != RING_MAGIC ||
        header->version != RING_VERSION || header->owner_pid <= 0 ||
        !geometry_ok(header->width, header->height, header->slots,
                     &expected_frame, map_size)) {
        return false;
    }
    return header->frame_size == (uint64_t)expected_frame;
}

bool krtsp_frame_attach(krtsp_frame **out, const char *name)
{
    krtsp_frame *frame;
    struct ring_header probe;
    struct stat info;
    void *mapping;
    size_t map_size;
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
    fd = shm_open(frame->name, O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        free(frame);
        return false;
    }
    /* Read the header before mapping: the full size is computed from it,
     * and mapping a bogus object at a bogus size is how a reader turns a
     * missing producer into a crash. */
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() ||
        (info.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) !=
            (S_IRUSR | S_IWUSR) ||
        !pread_full(fd, &probe, sizeof(probe)) ||
        !immutable_header_ok(&probe, &map_size) || info.st_size < 0 ||
        (uintmax_t)info.st_size != (uintmax_t)map_size) {
        (void)close(fd);
        free(frame);
        return false;
    }
    /* A live producer holds an exclusive flock for the object's lifetime.
     * If we can take it, this is a crash orphan and has nobody to publish
     * another frame; reject it instead of attaching to a frozen camera. */
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        (void)flock(fd, LOCK_UN);
        (void)close(fd);
        free(frame);
        return false;
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
        (void)close(fd);
        free(frame);
        return false;
    }
    mapping = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    (void)close(fd);
    if (mapping == MAP_FAILED) {
        free(frame);
        return false;
    }
    frame->header = (struct ring_header *)mapping;
    /* Geometry is immutable, but verify the mapped object again under its
     * own lock so replacement or half-initialization cannot race the probe. */
    if (!ring_lock(frame->header)) {
        (void)munmap(mapping, map_size);
        free(frame);
        return false;
    }
    {
        size_t mapped_size = 0u;
        bool valid = immutable_header_ok(frame->header, &mapped_size) &&
                     mapped_size == map_size &&
                     frame->header->width == probe.width &&
                     frame->header->height == probe.height &&
                     frame->header->slots == probe.slots;

        if (valid) {
            /* Claimed here, while the header is already locked and
             * already known good. */
            frame->reader = -1;
            for (int i = 0; i < frame->header->slots - 2; i++) {
                if (frame->header->reader_pid[i] > 0 &&
                    !pid_is_alive(frame->header->reader_pid[i])) {
                    frame->header->reader_pid[i] = 0;
                }
                if (frame->header->reader_pid[i] == 0 && frame->reader < 0) {
                    frame->header->reader_pid[i] = (int64_t)getpid();
                    frame->reader = i;
                }
            }
        }
        ring_unlock(frame->header);
        if (!valid) {
            (void)munmap(mapping, map_size);
            free(frame);
            return false;
        }
    }
    frame->slots = (uint8_t *)mapping + align_up(sizeof(struct ring_header));
    frame->map_size = map_size;
    frame->shared = true;
    frame->owns = false;   /* attaching never unlinks the producer's object */
    frame->held = -1;
    frame->lease = -1;
    frame->owner_fd = -1;
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
    krtsp_frame_release(frame);
    if (frame->shared && frame->reader >= 0 && frame->header != NULL) {
        if (ring_lock(frame->header)) {
            if (frame->reader < frame->header->slots - 2 &&
                frame->header->reader_pid[frame->reader] ==
                    (int64_t)getpid()) {
                frame->header->reader_pid[frame->reader] = 0;
            }
            ring_unlock(frame->header);
        }
        frame->reader = -1;
    }
    if (frame->shared) {
        if (frame->owns) {
            /* Unlink while the lifetime lock is still held.  Existing
             * mappings remain valid, but nobody can attach to stale state
             * and a successor can create a fresh object immediately. */
            (void)shm_unlink(frame->name);
        }
        (void)munmap(frame->header, frame->map_size);
        if (frame->owner_fd >= 0) {
            (void)close(frame->owner_fd);
        }
    } else {
        (void)pthread_mutex_destroy(&frame->header->lock);
        free(frame->header);
    }
    free(frame);
}

size_t krtsp_frame_readers(const krtsp_frame *frame)
{
    struct krtsp_frame *mutable_frame = (struct krtsp_frame *)frame;
    size_t total = 0u;

    if (frame == NULL || !frame->shared || frame->header == NULL) {
        return 0u;
    }
    if (!ring_lock(mutable_frame->header)) {
        return 0u;
    }
    for (int i = 0; i < frame->header->slots - 2; i++) {
        const int64_t pid = frame->header->reader_pid[i];

        if (pid <= 0) {
            continue;
        }
        /* Counted and swept in one pass: a reader that died holding a
         * slot must not keep a camera's detector running for ever. */
        if (!pid_is_alive(pid)) {
            mutable_frame->header->reader_pid[i] = 0;
            continue;
        }
        total++;
    }
    ring_unlock(mutable_frame->header);
    return total;
}

uint8_t *krtsp_frame_back(krtsp_frame *frame)
{
    uint8_t *buffer;

    if (frame == NULL || !frame->owns) {
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
    if (frame == NULL || !frame->owns || !ring_lock(frame->header)) {
        return;
    }
    {
        struct ring_header *header = frame->header;
        int next;

        next = pick_writable(header, header->writing);
        if (next < 0 && frame->shared) {
            /* A killed reader cannot execute release().  Reap its lease
             * only on saturation, keeping the normal publish path free of
             * per-frame process probes. */
            reclaim_dead_borrows(header);
            next = pick_writable(header, header->writing);
        }
        if (next < 0) {
            /* Cannot happen while borrow() honours the reservation below,
             * and dropping the frame is the safe response if it ever
             * does: overwriting a pinned slot would corrupt a reader. */
            ring_unlock(header);
            return;
        }
        /* A frame nobody borrowed is about to be replaced by a newer one.
         * Count it only after proving the exchange can complete. */
        if (header->newest >= 0 && !header->newest_taken) {
            replaced = true;
            header->dropped++;
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
    if (frame->shared) {
        int free_lease = -1;
        int max_readers = header->slots - 2;

        if (active_borrows(header) >= max_readers) {
            reclaim_dead_borrows(header);
        }
        if (active_borrows(header) >= max_readers) {
            ring_unlock(header);
            return NULL;
        }
        for (int index = 0; index < max_readers; ++index) {
            if (header->borrower_pid[index] <= 0) {
                free_lease = index;
                break;
            }
        }
        if (free_lease < 0) {
            ring_unlock(header);
            return NULL;
        }
        frame->held = header->newest;
        frame->lease = free_lease;
        /* slot first, PID last: the PID commits the lease.  Robust-lock
         * recovery can therefore rebuild pin[] after death at any store. */
        header->borrower_slot[free_lease] = frame->held;
        header->borrower_pid[free_lease] = (int64_t)getpid();
    } else {
        frame->held = header->newest;
    }
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
    {
        int held = frame->held;

        if (frame->shared && frame->lease >= 0 &&
            frame->lease < frame->header->slots - 2 &&
            frame->header->borrower_pid[frame->lease] ==
                (int64_t)getpid() &&
            frame->header->borrower_slot[frame->lease] == held) {
            /* Clear the authoritative lease before its derived pin count.
             * Robust recovery fixes the count if this process dies between
             * the stores.  A forked child cannot clear its parent's lease. */
            frame->header->borrower_pid[frame->lease] = 0;
            frame->header->borrower_slot[frame->lease] = -1;
            if (frame->header->pin[held] > 0) {
                frame->header->pin[held]--;
            }
        } else if (!frame->shared && frame->header->pin[held] > 0) {
            frame->header->pin[held]--;
        }
    }
    frame->held = -1;
    frame->lease = -1;
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
