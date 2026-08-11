/*
 * One camera as a supervised ffmpeg subprocess.
 *
 * Decode sources use two threads; record-only sources need just the first:
 *
 *   reader      blocks in read() on the child's stdout, assembles whole
 *               frames, publishes them.  Exits on EOF or error.
 *   supervisor  owns the child's lifecycle.  Spawns it, watches on a
 *               one-second tick, kills and restarts it, and applies the
 *               backoff.  This is the only thread that calls posix_spawn,
 *               waitpid or kill.
 *
 * A blocked reader is not a problem to be avoided; it is the normal state,
 * and the supervisor exists precisely to notice when it has been blocked
 * too long.  That is why the reader can afford to be a simple blocking
 * loop rather than a poll over descriptors.
 *
 * The supervisor distinguishes two failures that look identical from
 * inside the reader:
 *
 *   the child exited       waitpid() reaps it; read() returns EOF
 *   the child is wedged    read() blocks forever with no error, because
 *                          the camera is holding the TCP connection open
 *                          without sending anything
 *
 * Only the second needs a timer, and it is the one that actually happens.
 * posix_spawn() also avoids the unsafe post-fork work that a multithreaded
 * terminal would otherwise perform before exec.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kilix_rtsp.h"
#include "krtsp_exec.h"
#include "krtsp_source_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

struct krtsp_source {
    char url[512];
    krtsp_source_options options;
    char ffmpeg[256];
    bool legacy_timeout;
    char *owned_log_path;
    char *owned_record_dir;
    char *owned_record_pattern;

    krtsp_frame *frame;
    size_t frame_size;

    unsigned roles;
    bool segment_mkdir;
    char segment_list[512];        /* manifest ffmpeg rewrites per segment */
    /* The manifest's own mtime is wall-clock; when we last saw it change
     * is monotonic.  Only the second can be compared against the tick,
     * and mixing them is how a clock adjustment becomes a restart loop. */
    long long segment_mtime_ns;       /* supervisor thread only */
    _Atomic long long segment_seen_ms;
    _Atomic int record_status;

    pthread_t supervisor;
    pthread_t reader;
    bool supervisor_started;
    bool reader_started;

    /* Guards the child pid, the pipe, and the stop flag; also the
     * condition variable the interruptible sleep waits on. */
    pthread_mutex_t lock;
    pthread_cond_t wakeup;
    bool stopping;
    pid_t child;
    int pipe_read;

    _Atomic int status;
    _Atomic long long last_frame_ms;
    _Atomic long long decode_online_since_ms;
    _Atomic long long record_online_since_ms;

    _Atomic unsigned long long frames;
    _Atomic unsigned long long restarts;
    _Atomic unsigned long long stalls;
    _Atomic int consecutive_failures;
};

static long long monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (long long)now.tv_sec * 1000 + (long long)now.tv_nsec / 1000000;
}

void krtsp_source_options_init(krtsp_source_options *options)
{
    if (options == NULL) {
        return;
    }
    options->width = 0;
    options->height = 0;
    options->fps_cap = 0;
    options->pixfmt = KRTSP_PIXFMT_RGBA;
    options->low_latency = true;
    options->realtime = true;
    options->letterbox = false;
    options->stall_ms = 20000;
    options->grace_ms = 15000;
    options->backoff_min_ms = 1000;
    options->backoff_max_ms = 60000;
    options->stable_ms = 30000;
    options->max_consecutive_failures = 0;
    options->log_path = NULL;
    options->ffmpeg_path = NULL;
    options->roles = (unsigned)KRTSP_ROLE_DECODE;
    options->record_dir = NULL;
    options->record_pattern = NULL;
    options->segment_seconds = 0;
    options->record_audio = false;
    options->segment_stall_ms = 0;
}

const char *krtsp_status_name(krtsp_status status)
{
    switch (status) {
    case KRTSP_STARTING: return "starting";
    case KRTSP_ONLINE:   return "online";
    case KRTSP_STALE:    return "stale";
    case KRTSP_OFFLINE:  return "offline";
    case KRTSP_FAILED:   return "failed";
    default:             return "unknown";
    }
}

/* ---------------------------- ffmpeg probing ---------------------------- */

#define PROBE_CACHE_MAX 8
#define PROBE_LEGACY_TIMEOUT 1u
#define PROBE_SEGMENT_MKDIR 2u
#define PROBE_PATH_MAX 256

typedef struct probe_cache_entry {
    char path[PROBE_PATH_MAX];
    unsigned known;
    unsigned value;
    unsigned running;
} probe_cache_entry;

static pthread_mutex_t probe_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t probe_cache_ready = PTHREAD_COND_INITIALIZER;
static probe_cache_entry probe_cache[PROBE_CACHE_MAX];

static bool probe_segment_mkdir(const char *ffmpeg_path)
{
    char output[32768];
    char *argv[] = {
        (char *)ffmpeg_path, (char *)"-hide_banner", (char *)"-h",
        (char *)"muxer=segment", NULL
    };

    return krtsp_exec_capture(ffmpeg_path, argv, output, sizeof(output), 5000) ==
               0 &&
           strstr(output, "strftime_mkdir") != NULL;
}

static bool probe_legacy_timeout(const char *ffmpeg_path)
{
    char output[8192];
    char *argv[] = {
        (char *)ffmpeg_path, (char *)"-hide_banner", (char *)"-version", NULL
    };
    const char *line;
    int major = 0;

    if (krtsp_exec_capture(ffmpeg_path, argv, output, sizeof(output), 5000) !=
        0) {
        return false;
    }
    line = output;
    while (line[0] != '\0') {
        const char *next = strchr(line, '\n');
        size_t length = next != NULL ? (size_t)(next - line) : strlen(line);

        if (length >= 11u && strncmp(line, "libavformat", 11u) == 0) {
            const char *scan = line + 11;
            char *end;
            long parsed;

            while ((size_t)(scan - line) < length &&
                   (*scan < '0' || *scan > '9')) {
                scan++;
            }
            if ((size_t)(scan - line) >= length) {
                break;
            }
            errno = 0;
            parsed = strtol(scan, &end, 10);
            if (errno == 0 && end != scan &&
                (size_t)(end - line) <= length && parsed > 0 &&
                parsed <= INT_MAX) {
                major = (int)parsed;
            }
            break;
        }
        if (next == NULL) {
            break;
        }
        line = next + 1;
    }
    return major > 0 && major < 59;
}

static bool cached_probe(const char *path, unsigned capability,
                         bool (*probe)(const char *))
{
    probe_cache_entry *entry = NULL;
    bool value;

    if (strlen(path) >= PROBE_PATH_MAX) {
        return probe(path);
    }
    (void)pthread_mutex_lock(&probe_cache_lock);
    for (;;) {
        probe_cache_entry *empty = NULL;

        entry = NULL;
        for (size_t index = 0u; index < PROBE_CACHE_MAX; ++index) {
            probe_cache_entry *candidate = &probe_cache[index];

            if (candidate->path[0] == '\0') {
                if (empty == NULL) {
                    empty = candidate;
                }
            } else if (strcmp(candidate->path, path) == 0) {
                entry = candidate;
                break;
            }
        }
        if (entry == NULL) {
            if (empty == NULL) {
                (void)pthread_mutex_unlock(&probe_cache_lock);
                return probe(path);
            }
            entry = empty;
            (void)snprintf(entry->path, sizeof(entry->path), "%s", path);
        }
        if ((entry->known & capability) != 0u) {
            value = (entry->value & capability) != 0u;
            (void)pthread_mutex_unlock(&probe_cache_lock);
            return value;
        }
        if ((entry->running & capability) == 0u) {
            entry->running |= capability;
            break;
        }
        /* Another source is already asking this exact question.  Wait for
         * its result; unrelated paths and capabilities probe concurrently. */
        (void)pthread_cond_wait(&probe_cache_ready, &probe_cache_lock);
    }

    (void)pthread_mutex_unlock(&probe_cache_lock);
    value = probe(path);

    (void)pthread_mutex_lock(&probe_cache_lock);
    entry->known |= capability;
    entry->running &= ~capability;
    if (value) {
        entry->value |= capability;
    } else {
        entry->value &= ~capability;
    }
    (void)pthread_cond_broadcast(&probe_cache_ready);
    (void)pthread_mutex_unlock(&probe_cache_lock);
    return value;
}

/*
 * Does this ffmpeg's segment muxer create the directories a strftime
 * pattern names?
 *
 * It matters because the natural recording layout is a date hierarchy -
 * one directory per day, one per hour - and the segment muxer will not
 * make those itself unless it has -strftime_mkdir.  Without it, every
 * segment fails with "No such file or directory" the moment the hour
 * rolls over, and the first failure is at whatever hour the operator was
 * not watching.
 *
 * Not universal: ffmpeg 5.1 has -strftime but not -strftime_mkdir.
 * Probed rather than assumed, because the failure is silent until it is
 * an entire missing hour of footage.
 */
bool krtsp_ffmpeg_supports_segment_mkdir(const char *ffmpeg_path)
{
    if (ffmpeg_path == NULL || ffmpeg_path[0] == '\0') {
        ffmpeg_path = "ffmpeg";
    }
    return cached_probe(ffmpeg_path, PROBE_SEGMENT_MKDIR,
                        probe_segment_mkdir);
}

bool krtsp_ffmpeg_needs_legacy_timeout(const char *ffmpeg_path)
{
    if (ffmpeg_path == NULL || ffmpeg_path[0] == '\0') {
        ffmpeg_path = "ffmpeg";
    }
    return cached_probe(ffmpeg_path, PROBE_LEGACY_TIMEOUT,
                        probe_legacy_timeout);
}

/* Newest segment activity, from the manifest ffmpeg rewrites as each
 * segment completes.  One stat, whatever shape the recording tree is. */
static long long segment_mtime_ns(const char *path)
{
    struct stat info;

    if (path == NULL || path[0] == '\0' || stat(path, &info) != 0) {
        return 0;
    }
    return (long long)info.st_mtim.tv_sec * 1000000000LL +
           (long long)info.st_mtim.tv_nsec;
}

/* ------------------------------- spawning ------------------------------- */

/* Read exactly `size` bytes unless the stream ends.
 *
 * Resuming a short read is not an optimization.  A pipe splits writes
 * wherever it likes, so partial frames are normal; a reader that
 * discards the remainder is misaligned by that many bytes for every
 * subsequent frame, and the result looks like a codec bug rather than
 * like the framing error it is. */
static bool read_full(int fd, uint8_t *buffer, size_t size)
{
    size_t total = 0u;

    while (total < size) {
        size_t remaining = size - total;
        ssize_t count;

        if (remaining > (size_t)SSIZE_MAX) {
            remaining = (size_t)SSIZE_MAX;
        }
        count = read(fd, buffer + total, remaining);

        if (count > 0) {
            total += (size_t)count;
        } else if (count == 0) {
            return false;   /* EOF */
        } else if (errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static void fill_args_request(const krtsp_source *source,
                              krtsp_args_request *request)
{
    krtsp_args_request_init(request);
    request->url = source->url;
    request->width = source->options.width;
    request->height = source->options.height;
    request->fps_cap = source->options.fps_cap;
    request->pixfmt = source->options.pixfmt;
    request->low_latency = source->options.low_latency;
    request->realtime = source->options.realtime;
    request->seek_seconds = source->options.seek_seconds;
    request->letterbox = source->options.letterbox;
    request->legacy_timeout_flag = source->legacy_timeout;
    request->roles = source->roles;
    request->record_dir = source->options.record_dir;
    request->record_pattern = source->options.record_pattern;
    request->segment_seconds = source->options.segment_seconds;
    request->record_audio = source->options.record_audio;
    request->segment_mkdir = source->segment_mkdir;
    request->segment_list = source->segment_list[0] != '\0'
                                ? source->segment_list
                                : NULL;
}

static bool move_above_stdio(int *fd)
{
    int moved;

    if (*fd > STDERR_FILENO) {
        return true;
    }
    moved = fcntl(*fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (moved < 0) {
        return false;
    }
    (void)close(*fd);
    *fd = moved;
    return true;
}

static bool pipe_cloexec(int fds[2])
{
#ifdef __linux__
    return pipe2(fds, O_CLOEXEC) == 0;
#else
    if (pipe(fds) != 0) {
        return false;
    }
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0) {
        int saved = errno;

        (void)close(fds[0]);
        (void)close(fds[1]);
        errno = saved;
        return false;
    }
    return true;
#endif
}

static int open_log_fd(const char *path)
{
    int fd = -1;

    if (path != NULL) {
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    }
    if (fd < 0) {
        fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    }
    if (fd >= 0 && !move_above_stdio(&fd)) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static pid_t spawn_child(krtsp_source *source, int *read_fd)
{
    char *argv[KRTSP_ARGV_MAX];
    char storage[KRTSP_ARGV_STORAGE_MAX];
    krtsp_args_request request;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    sigset_t defaults;
    sigset_t mask;
    int fds[2] = {-1, -1};
    int err_fd = -1;
    bool decode = (source->roles & (unsigned)KRTSP_ROLE_DECODE) != 0u;
    short flags;
    pid_t pid;
    int rc;

    *read_fd = -1;
    fill_args_request(source, &request);

    if (krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                         sizeof(storage)) == 0u) {
        return -1;
    }
    argv[0] = source->ffmpeg;

    if (decode && (!pipe_cloexec(fds) || !move_above_stdio(&fds[0]) ||
                   !move_above_stdio(&fds[1]))) {
        goto fail;
    }
    err_fd = open_log_fd(source->options.log_path);
    if (err_fd < 0 || posix_spawn_file_actions_init(&actions) != 0) {
        goto fail;
    }
    if (posix_spawnattr_init(&attributes) != 0) {
        (void)posix_spawn_file_actions_destroy(&actions);
        goto fail;
    }
    if (sigemptyset(&defaults) != 0 || sigaddset(&defaults, SIGPIPE) != 0 ||
        sigaddset(&defaults, SIGHUP) != 0 ||
        sigaddset(&defaults, SIGINT) != 0 ||
        sigaddset(&defaults, SIGQUIT) != 0 ||
        sigaddset(&defaults, SIGTERM) != 0 || sigemptyset(&mask) != 0 ||
        posix_spawnattr_setsigdefault(&attributes, &defaults) != 0 ||
        posix_spawnattr_setsigmask(&attributes, &mask) != 0) {
        goto actions_fail;
    }
    flags = (short)(POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    if (posix_spawnattr_setflags(&attributes, flags) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                         O_RDONLY, 0) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, err_fd, STDERR_FILENO) != 0 ||
        posix_spawn_file_actions_addclose(&actions, err_fd) != 0) {
        goto actions_fail;
    }
    if (decode) {
        if (posix_spawn_file_actions_adddup2(&actions, fds[1],
                                             STDOUT_FILENO) != 0 ||
            posix_spawn_file_actions_addclose(&actions, fds[0]) != 0 ||
            posix_spawn_file_actions_addclose(&actions, fds[1]) != 0) {
            goto actions_fail;
        }
    } else if (posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                                 "/dev/null", O_WRONLY, 0) !=
               0) {
        goto actions_fail;
    }

    rc = posix_spawnp(&pid, source->ffmpeg, &actions, &attributes, argv,
                      environ);
    (void)posix_spawnattr_destroy(&attributes);
    (void)posix_spawn_file_actions_destroy(&actions);
    (void)close(err_fd);
    err_fd = -1;
    if (rc != 0) {
        errno = rc;
        goto fail;
    }
    if (decode) {
        (void)close(fds[1]);
        *read_fd = fds[0];
    }
    return pid;

actions_fail:
    (void)posix_spawnattr_destroy(&attributes);
    (void)posix_spawn_file_actions_destroy(&actions);
fail:
    if (err_fd >= 0) {
        (void)close(err_fd);
    }
    if (fds[0] >= 0) {
        (void)close(fds[0]);
    }
    if (fds[1] >= 0) {
        (void)close(fds[1]);
    }
    return -1;
}

static pid_t waitpid_nointr(pid_t pid, int *status, int options)
{
    pid_t waited;

    do {
        waited = waitpid(pid, status, options);
    } while (waited < 0 && errno == EINTR);
    return waited;
}

/* SIGTERM, then SIGKILL: ffmpeg holding an RTSP session open does not
 * always exit promptly, and the supervisor must not block on it. */
static void terminate_child(pid_t pid)
{
    int waited_ms = 0;

    if (pid <= 0) {
        return;
    }
    (void)kill(pid, SIGTERM);
    while (waited_ms < 2000) {
        pid_t waited = waitpid_nointr(pid, NULL, WNOHANG);

        if (waited == pid) {
            return;
        }
        if (waited < 0) {
            return;
        }
        struct timespec pause = {0, 50 * 1000000L};
        (void)nanosleep(&pause, NULL);
        waited_ms += 50;
    }
    (void)kill(pid, SIGKILL);
    (void)waitpid_nointr(pid, NULL, 0);
}

/* ------------------------------- threads -------------------------------- */

static void *reader_main(void *argument)
{
    krtsp_source *source = argument;

    for (;;) {
        uint8_t *buffer;
        int fd;

        pthread_mutex_lock(&source->lock);
        fd = source->pipe_read;
        pthread_mutex_unlock(&source->lock);
        if (fd < 0) {
            break;
        }
        /* Re-fetch after every publish: publishing hands the buffer to
         * the consumer and gives back a different one. */
        buffer = krtsp_frame_back(source->frame);
        if (buffer == NULL) {
            break;
        }
        if (!read_full(fd, buffer, source->frame_size)) {
            break;
        }
        krtsp_frame_publish(source->frame, NULL);
        atomic_fetch_add(&source->frames, 1ull);
        {
            long long now = monotonic_ms();
            long long not_online = 0;

            atomic_store(&source->last_frame_ms, now);
            (void)atomic_compare_exchange_strong(
                &source->decode_online_since_ms, &not_online, now);
        }
        atomic_store(&source->status, (int)KRTSP_ONLINE);
    }
    return NULL;
}

/* Sleep until `deadline_ms` or until stopping, whichever comes first. */
static bool sleep_until_stop(krtsp_source *source, int milliseconds)
{
    struct timespec deadline;

    if (milliseconds <= 0) {
        return false;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        bool stopping;

        pthread_mutex_lock(&source->lock);
        stopping = source->stopping;
        pthread_mutex_unlock(&source->lock);
        return stopping;
    }
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&source->lock);
    while (!source->stopping) {
        int rc = pthread_cond_timedwait(&source->wakeup, &source->lock,
                                        &deadline);

        if (rc != 0) {
            break;
        }
    }
    bool stopping = source->stopping;
    pthread_mutex_unlock(&source->lock);
    return stopping;
}

static bool is_stopping(krtsp_source *source)
{
    bool stopping;

    pthread_mutex_lock(&source->lock);
    stopping = source->stopping;
    pthread_mutex_unlock(&source->lock);
    return stopping;
}

static void set_role_status(krtsp_source *source, krtsp_status status)
{
    if ((source->roles & (unsigned)KRTSP_ROLE_DECODE) != 0u) {
        atomic_store(&source->status, (int)status);
    }
    if ((source->roles & (unsigned)KRTSP_ROLE_RECORD) != 0u) {
        atomic_store(&source->record_status, (int)status);
    }
}

static int next_backoff(int current, int maximum)
{
    return current > maximum / 2 ? maximum : current * 2;
}

static bool roles_were_stable(const krtsp_source *source, long long now)
{
    if ((source->roles & (unsigned)KRTSP_ROLE_DECODE) != 0u) {
        long long since = atomic_load(&source->decode_online_since_ms);

        if (since <= 0 || now - since < source->options.stable_ms) {
            return false;
        }
    }
    if ((source->roles & (unsigned)KRTSP_ROLE_RECORD) != 0u) {
        long long since = atomic_load(&source->record_online_since_ms);

        if (since <= 0 || now - since < source->options.stable_ms) {
            return false;
        }
    }
    return true;
}

static void *supervisor_main(void *argument)
{
    krtsp_source *source = argument;
    int backoff = source->options.backoff_min_ms;
    bool first = true;

    while (!is_stopping(source)) {
        int read_fd = -1;
        pid_t pid;
        long long started;
        bool stalled = false;

        set_role_status(source, KRTSP_STARTING);
        atomic_store(&source->decode_online_since_ms, 0);
        atomic_store(&source->record_online_since_ms, 0);
        atomic_store(&source->segment_seen_ms, 0);
        if ((source->roles & (unsigned)KRTSP_ROLE_RECORD) != 0u) {
            /* An old manifest proves only that a previous run recorded.
             * Snapshot it before spawning and require this run to change it. */
            source->segment_mtime_ns =
                segment_mtime_ns(source->segment_list);
        }
        pid = spawn_child(source, &read_fd);
        if (pid < 0) {
            set_role_status(source, KRTSP_OFFLINE);
            atomic_fetch_add(&source->consecutive_failures, 1);
            if (source->options.max_consecutive_failures > 0 &&
                atomic_load(&source->consecutive_failures) >=
                    source->options.max_consecutive_failures) {
                set_role_status(source, KRTSP_FAILED);
                break;
            }
            if (sleep_until_stop(source, backoff)) {
                break;
            }
            backoff = next_backoff(backoff, source->options.backoff_max_ms);
            continue;
        }
        if (!first) {
            atomic_fetch_add(&source->restarts, 1ull);
        }
        first = false;

        started = monotonic_ms();
        atomic_store(&source->last_frame_ms, started);

        pthread_mutex_lock(&source->lock);
        source->child = pid;
        source->pipe_read = read_fd;
        pthread_mutex_unlock(&source->lock);

        if ((source->roles & (unsigned)KRTSP_ROLE_DECODE) != 0u &&
            pthread_create(&source->reader, NULL, reader_main, source) != 0) {
            terminate_child(pid);
            (void)close(read_fd);
            pthread_mutex_lock(&source->lock);
            source->child = -1;
            source->pipe_read = -1;
            pthread_mutex_unlock(&source->lock);
            set_role_status(source, KRTSP_OFFLINE);
            atomic_fetch_add(&source->consecutive_failures, 1);
            if (source->options.max_consecutive_failures > 0 &&
                atomic_load(&source->consecutive_failures) >=
                    source->options.max_consecutive_failures) {
                set_role_status(source, KRTSP_FAILED);
                break;
            }
            if (sleep_until_stop(source, backoff)) {
                break;
            }
            backoff = next_backoff(backoff,
                                   source->options.backoff_max_ms);
            continue;
        }
        source->reader_started =
            (source->roles & (unsigned)KRTSP_ROLE_DECODE) != 0u;

        /* One-second watch, the interval Frigate settled on: slow enough
         * to be free, fast enough that a wedge is noticed in time. */
        for (;;) {
            long long now;
            int status_value;

            if (sleep_until_stop(source, 1000)) {
                break;
            }
            pid_t waited = waitpid_nointr(pid, &status_value, WNOHANG);

            if (waited == pid || (waited < 0 && errno == ECHILD)) {
                /* Exited on its own; the reader will see EOF. */
                pthread_mutex_lock(&source->lock);
                source->child = -1;
                pthread_mutex_unlock(&source->lock);
                break;
            }
            now = monotonic_ms();

            /*
             * Recording liveness is its own question.  A camera can
             * deliver frames while its segmenter is stuck on a full disk
             * or an unwritable directory, and can keep writing segments
             * after the decode pipe has stopped.  One timer cannot report
             * both, so each sink gets its own.
             */
            if ((source->roles & (unsigned)KRTSP_ROLE_RECORD) != 0u) {
                long long mtime = segment_mtime_ns(source->segment_list);
                long long segment_stall = source->options.segment_stall_ms;
                long long seen;

                if (segment_stall <= 0) {
                    int seconds = source->options.segment_seconds > 0
                                      ? source->options.segment_seconds
                                      : 10;
                    /* Three rotations: tolerant of one missed segment,
                     * intolerant of a dead sink. */
                    segment_stall = (long long)seconds * 3000;
                }
                if (mtime != 0 && mtime != source->segment_mtime_ns) {
                    long long not_online = 0;

                    source->segment_mtime_ns = mtime;
                    atomic_store(&source->segment_seen_ms, now);
                    (void)atomic_compare_exchange_strong(
                        &source->record_online_since_ms, &not_online, now);
                    atomic_store(&source->record_status, (int)KRTSP_ONLINE);
                }
                seen = atomic_load(&source->segment_seen_ms);
                if (seen == 0) {
                    /* Nothing yet this run: the grace period runs from
                     * the spawn, since a fresh segmenter has legitimately
                     * written nothing. */
                    seen = started;
                }
                if (now - started > source->options.grace_ms &&
                    now - seen > segment_stall) {
                    atomic_store(&source->record_status, (int)KRTSP_STALE);
                    atomic_fetch_add(&source->stalls, 1ull);
                    stalled = true;
                    break;
                }
            }

            if ((source->roles & (unsigned)KRTSP_ROLE_DECODE) != 0u &&
                now - started > source->options.grace_ms &&
                now - atomic_load(&source->last_frame_ms) >
                    source->options.stall_ms) {
                /* Alive but silent: the wedged-camera case.  Nothing
                 * below the application layer reports this. */
                atomic_store(&source->status, (int)KRTSP_STALE);
                atomic_fetch_add(&source->stalls, 1ull);
                stalled = true;
                break;
            }
        }

        /*
         * Teardown order matters, and there are two orderings that look
         * plausible and only one that is correct.
         *
         * Joining the reader first deadlocks on exactly the wedged camera
         * this supervisor exists for: the reader is parked in read() on a
         * pipe nothing is writing to, and will be until something ends
         * the child.
         *
         * So kill the child first.  terminate_child() does not return
         * until the process is reaped, and a reaped child's end of the
         * pipe is closed, so the reader's read() returns 0 and the thread
         * leaves on its own.  Only then is closing the descriptor safe.
         *
         * Closing before the join - which is what this did originally -
         * yanks the descriptor out from under a thread that may be inside
         * read() on it.  ThreadSanitizer flags it, and the practical
         * hazard is worse than the report: once closed, that number is
         * free for any other thread's open() to claim, and the reader
         * would then read a frame's worth of some unrelated file.
         */
        pthread_mutex_lock(&source->lock);
        int fd_to_close = source->pipe_read;
        source->pipe_read = -1;
        pid_t to_kill = source->child;
        source->child = -1;
        pthread_mutex_unlock(&source->lock);

        if (to_kill > 0) {
            terminate_child(to_kill);
        }
        if (source->reader_started) {
            (void)pthread_join(source->reader, NULL);
            source->reader_started = false;
        }
        if (fd_to_close >= 0) {
            (void)close(fd_to_close);
        }
        if (is_stopping(source)) {
            break;
        }

        set_role_status(source, KRTSP_OFFLINE);

        /* Reset the backoff only after a sustained online period.  On the
         * first frame instead, a camera that delivers one frame and dies
         * would spin in a tight restart loop forever. */
        if (!stalled && roles_were_stable(source, monotonic_ms())) {
            backoff = source->options.backoff_min_ms;
            atomic_store(&source->consecutive_failures, 0);
        } else {
            atomic_fetch_add(&source->consecutive_failures, 1);
        }

        if (source->options.max_consecutive_failures > 0 &&
            atomic_load(&source->consecutive_failures) >=
                source->options.max_consecutive_failures) {
            set_role_status(source, KRTSP_FAILED);
            break;
        }
        if (sleep_until_stop(source, backoff)) {
            break;
        }
        backoff = next_backoff(backoff, source->options.backoff_max_ms);
    }
    return NULL;
}

/* -------------------------------- public -------------------------------- */

static void free_source_storage(krtsp_source *source)
{
    if (source == NULL) {
        return;
    }
    krtsp_frame_free(source->frame);
    free(source->owned_record_pattern);
    free(source->owned_record_dir);
    free(source->owned_log_path);
    free(source);
}

static bool copy_option_string(char **owned, const char **option,
                               const char *value)
{
    if (value == NULL) {
        *owned = NULL;
        *option = NULL;
        return true;
    }
    *owned = strdup(value);
    if (*owned == NULL) {
        return false;
    }
    *option = *owned;
    return true;
}

bool krtsp_source_start(
    krtsp_source **out,
    const char *url,
    const krtsp_source_options *options)
{
    krtsp_source_options defaults;
    krtsp_source *source;
    const char *binary;
    unsigned roles;

    if (out == NULL) {
        return false;
    }
    *out = NULL;
    if (url == NULL || url[0] == '\0') {
        return false;
    }
    if (options == NULL) {
        krtsp_source_options_init(&defaults);
        options = &defaults;
    }
    roles = options->roles != 0u ? options->roles
                                 : (unsigned)KRTSP_ROLE_DECODE;
    if ((roles & ~(unsigned)(KRTSP_ROLE_DECODE | KRTSP_ROLE_RECORD)) != 0u) {
        return false;
    }
    /* Frame geometry and pixel format are meaningful only to the decode
     * sink; a record-only source allocates no decoded frame. */
    if ((roles & (unsigned)KRTSP_ROLE_DECODE) != 0u &&
        (options->width <= 0 || options->height <= 0 ||
         (options->pixfmt != KRTSP_PIXFMT_RGBA &&
          options->pixfmt != KRTSP_PIXFMT_BGRA))) {
        return false;
    }
    if ((roles & (unsigned)KRTSP_ROLE_RECORD) != 0u &&
        (options->record_dir == NULL || options->record_dir[0] == '\0')) {
        return false;
    }
    if (options->fps_cap < 0 || options->segment_seconds < 0 ||
        options->segment_stall_ms < 0 || options->stall_ms <= 0 ||
        options->grace_ms < 0 || options->stable_ms < 0 ||
        options->max_consecutive_failures < 0 ||
        options->backoff_min_ms <= 0 ||
        options->backoff_max_ms < options->backoff_min_ms) {
        return false;
    }
    if (strlen(url) >= sizeof(source->url)) {
        return false;
    }

    source = calloc(1u, sizeof(*source));
    if (source == NULL) {
        return false;
    }
    (void)snprintf(source->url, sizeof(source->url), "%s", url);
    source->options = *options;
    source->roles = roles;
    source->child = -1;
    source->pipe_read = -1;
    if (!copy_option_string(&source->owned_log_path,
                            &source->options.log_path, options->log_path) ||
        !copy_option_string(&source->owned_record_dir,
                            &source->options.record_dir,
                            options->record_dir) ||
        !copy_option_string(&source->owned_record_pattern,
                            &source->options.record_pattern,
                            options->record_pattern)) {
        free_source_storage(source);
        return false;
    }

    binary = options->ffmpeg_path;
    if (binary == NULL || binary[0] == '\0') {
        binary = getenv("KILIX_RTSP_FFMPEG");
    }
    if (binary == NULL || binary[0] == '\0') {
        binary = "ffmpeg";
    }
    if (strlen(binary) >= sizeof(source->ffmpeg)) {
        free_source_storage(source);
        return false;
    }
    (void)snprintf(source->ffmpeg, sizeof(source->ffmpeg), "%s", binary);
    source->legacy_timeout = krtsp_ffmpeg_needs_legacy_timeout(source->ffmpeg);

    if ((source->roles & (unsigned)KRTSP_ROLE_RECORD) != 0u) {
        /* Probed, not assumed: ffmpeg 5.1 has -strftime but not
         * -strftime_mkdir, and passing a flag the binary lacks fails the
         * spawn outright. */
        source->segment_mkdir =
            krtsp_ffmpeg_supports_segment_mkdir(source->ffmpeg);
        int printed = snprintf(source->segment_list,
                               sizeof(source->segment_list), "%s/.segments",
                               source->options.record_dir);

        if (printed < 0 || (size_t)printed >= sizeof(source->segment_list)) {
            free_source_storage(source);
            return false;
        }
        atomic_store(&source->record_status, (int)KRTSP_STARTING);
    } else {
        atomic_store(&source->record_status, (int)KRTSP_OFFLINE);
    }

    /* A too-long URL/path combination is an argument error, not a camera
     * outage to retry forever from the supervisor thread. */
    {
        krtsp_args_request request;
        char *argv[KRTSP_ARGV_MAX];
        char storage[KRTSP_ARGV_STORAGE_MAX];

        fill_args_request(source, &request);
        if (krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                             sizeof(storage)) == 0u) {
            free_source_storage(source);
            return false;
        }
    }

    if ((source->roles & (unsigned)KRTSP_ROLE_DECODE) != 0u) {
        if (!krtsp_frame_init(&source->frame, source->options.width,
                              source->options.height)) {
            free_source_storage(source);
            return false;
        }
        source->frame_size = krtsp_frame_size(source->frame);
    }

    if (pthread_mutex_init(&source->lock, NULL) != 0) {
        free_source_storage(source);
        return false;
    }
    {
        pthread_condattr_t attributes;
        bool initialized = pthread_condattr_init(&attributes) == 0;
        bool ready = initialized;

        if (ready) {
            ready = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) == 0;
        }
        if (!ready || pthread_cond_init(&source->wakeup, &attributes) != 0) {
            if (initialized) {
                (void)pthread_condattr_destroy(&attributes);
            }
            (void)pthread_mutex_destroy(&source->lock);
            free_source_storage(source);
            return false;
        }
        (void)pthread_condattr_destroy(&attributes);
    }
    atomic_store(&source->status, (int)KRTSP_STARTING);

    if (pthread_create(&source->supervisor, NULL, supervisor_main,
                       source) != 0) {
        (void)pthread_cond_destroy(&source->wakeup);
        (void)pthread_mutex_destroy(&source->lock);
        free_source_storage(source);
        return false;
    }
    source->supervisor_started = true;
    *out = source;
    return true;
}

void krtsp_source_stop(krtsp_source *source)
{
    if (source == NULL) {
        return;
    }
    pthread_mutex_lock(&source->lock);
    source->stopping = true;
    pthread_cond_broadcast(&source->wakeup);
    /* Kill the child from here too: the supervisor may be inside its
     * one-second wait, and the reader is blocked on a pipe that only the
     * child's death will close. */
    if (source->child > 0) {
        (void)kill(source->child, SIGTERM);
    }
    pthread_mutex_unlock(&source->lock);

    if (source->supervisor_started) {
        (void)pthread_join(source->supervisor, NULL);
        source->supervisor_started = false;
    }
    (void)pthread_cond_destroy(&source->wakeup);
    (void)pthread_mutex_destroy(&source->lock);
    free_source_storage(source);
}

const uint8_t *krtsp_source_borrow(krtsp_source *source, int *age_ms)
{
    return krtsp_source_borrow_latest(source, NULL, age_ms);
}

const uint8_t *krtsp_source_borrow_latest(
    krtsp_source *source, uint64_t *sequence, int *age_ms)
{
    if (source == NULL) {
        return NULL;
    }
    return krtsp_frame_borrow(source->frame, sequence, age_ms);
}

void krtsp_source_release(krtsp_source *source)
{
    if (source == NULL) {
        return;
    }
    krtsp_frame_release(source->frame);
}

krtsp_status krtsp_source_role_status(
    const krtsp_source *source, krtsp_role role)
{
    if (source == NULL) {
        return KRTSP_FAILED;
    }
    if (role != KRTSP_ROLE_DECODE && role != KRTSP_ROLE_RECORD) {
        return KRTSP_FAILED;
    }
    if ((source->roles & (unsigned)role) == 0u) {
        /* Not running this role.  Reporting it offline is honest;
         * reporting it online would let a caller believe an archive
         * exists that nothing is writing. */
        return KRTSP_OFFLINE;
    }
    if (role == KRTSP_ROLE_RECORD) {
        return (krtsp_status)atomic_load(&source->record_status);
    }
    return (krtsp_status)atomic_load(&source->status);
}

/* Worse-of ordering.  A source with a healthy pipe and a wedged segmenter
 * is not "online", and saying so is the whole reason health is tracked
 * per role. */
static int status_severity(krtsp_status status)
{
    switch (status) {
    case KRTSP_ONLINE:   return 0;
    case KRTSP_STARTING: return 1;
    case KRTSP_STALE:    return 2;
    case KRTSP_OFFLINE:  return 3;
    case KRTSP_FAILED:   return 4;
    default:             return 5;
    }
}

krtsp_status krtsp_source_status(const krtsp_source *source)
{
    krtsp_status worst = KRTSP_ONLINE;
    bool any = false;

    if (source == NULL) {
        return KRTSP_FAILED;
    }
    if ((source->roles & (unsigned)KRTSP_ROLE_DECODE) != 0u) {
        worst = (krtsp_status)atomic_load(
            &((krtsp_source *)source)->status);
        any = true;
    }
    if ((source->roles & (unsigned)KRTSP_ROLE_RECORD) != 0u) {
        krtsp_status record = (krtsp_status)atomic_load(
            &((krtsp_source *)source)->record_status);

        if (!any || status_severity(record) > status_severity(worst)) {
            worst = record;
        }
        any = true;
    }
    return any ? worst : KRTSP_FAILED;
}

void krtsp_source_get_stats(
    const krtsp_source *source, krtsp_source_stats *out)
{
    krtsp_source *mutable_source;
    uint64_t published = 0u;
    uint64_t dropped = 0u;

    if (source == NULL || out == NULL) {
        return;
    }
    mutable_source = (krtsp_source *)source;
    krtsp_frame_stats(mutable_source->frame, &published, &dropped);
    out->frames = atomic_load(&mutable_source->frames);
    out->dropped = dropped;
    out->restarts = atomic_load(&mutable_source->restarts);
    out->stalls = atomic_load(&mutable_source->stalls);
    out->consecutive_failures =
        atomic_load(&mutable_source->consecutive_failures);
}

uint64_t krtsp_source_frame_count(const krtsp_source *source)
{
    return source != NULL
               ? (uint64_t)atomic_load(
                     &((krtsp_source *)source)->frames)
               : 0u;
}

int krtsp_source_frame_age_ms(const krtsp_source *source)
{
    long long age;

    if (source == NULL || krtsp_source_frame_count(source) == 0u) {
        return -1;
    }
    age = monotonic_ms() -
          atomic_load(&((krtsp_source *)source)->last_frame_ms);
    if (age < 0) {
        age = 0;
    }
    return age > INT_MAX ? INT_MAX : (int)age;
}
