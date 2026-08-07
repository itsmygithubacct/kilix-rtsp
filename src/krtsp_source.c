/*
 * One camera as a supervised ffmpeg subprocess.
 *
 * Two threads per source:
 *
 *   reader      blocks in read() on the child's stdout, assembles whole
 *               frames, publishes them.  Exits on EOF or error.
 *   supervisor  owns the child's lifecycle.  Spawns it, watches on a
 *               one-second tick, kills and restarts it, and applies the
 *               backoff.  This is the only thread that calls fork/exec,
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
 */

#include "kilix_rtsp.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct krtsp_source {
    char url[512];
    krtsp_source_options options;
    char ffmpeg[256];
    bool legacy_timeout;

    krtsp_frame *frame;
    size_t frame_size;

    unsigned roles;
    bool segment_mkdir;
    char segment_list[512];        /* manifest ffmpeg rewrites per segment */
    /* The manifest's own mtime is wall-clock; when we last saw it change
     * is monotonic.  Only the second can be compared against the tick,
     * and mixing them is how a clock adjustment becomes a restart loop. */
    long long segment_mtime;          /* supervisor thread only */
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
    char command[512];
    char line[256];
    FILE *pipe_handle;
    bool supported = false;

    if (ffmpeg_path == NULL || ffmpeg_path[0] == '\0') {
        ffmpeg_path = "ffmpeg";
    }
    if (snprintf(command, sizeof(command),
                 "%s -hide_banner -h muxer=segment 2>/dev/null",
                 ffmpeg_path) < 0) {
        return false;
    }
    pipe_handle = popen(command, "r");
    if (pipe_handle == NULL) {
        return false;
    }
    /* Bounded for the same reason as the version probe: a binary that is
     * not ffmpeg may print forever. */
    for (int line_number = 0; line_number < 256; ++line_number) {
        if (fgets(line, sizeof(line), pipe_handle) == NULL) {
            break;
        }
        if (strstr(line, "strftime_mkdir") != NULL) {
            supported = true;
            break;
        }
    }
    (void)pclose(pipe_handle);
    return supported;
}

bool krtsp_ffmpeg_needs_legacy_timeout(const char *ffmpeg_path)
{
    char command[512];
    char line[256];
    FILE *pipe_handle;
    bool legacy = false;
    int major = 0;

    if (ffmpeg_path == NULL || ffmpeg_path[0] == '\0') {
        ffmpeg_path = "ffmpeg";
    }
    /* Only the version banner is needed, and the binary name comes from
     * configuration rather than from a stream, so this is not a shell
     * injection surface in the way a URL would be. */
    if (snprintf(command, sizeof(command),
                 "%s -hide_banner -version 2>/dev/null", ffmpeg_path) < 0) {
        return false;
    }
    pipe_handle = popen(command, "r");
    if (pipe_handle == NULL) {
        return false;
    }
    /* Bounded: a binary that is not ffmpeg may produce output forever,
     * and a probe that can hang is worse than a probe that guesses. */
    for (int line_number = 0; line_number < 64; ++line_number) {
        if (fgets(line, sizeof(line), pipe_handle) == NULL) {
            break;
        }
        if (strncmp(line, "libavformat", 11u) == 0) {
            const char *scan = line + 11;

            while (*scan != '\0' && (*scan < '0' || *scan > '9')) {
                scan++;
            }
            major = atoi(scan);
            break;
        }
    }
    (void)pclose(pipe_handle);

    /* ffmpeg renamed the RTSP socket timeout at libavformat 59.  A
     * version we could not read is left on the modern spelling: guessing
     * the old one on a new binary is an immediate hard error, while the
     * reverse only matters on hosts old enough to be rare. */
    if (major > 0 && major < 59) {
        legacy = true;
    }
    return legacy;
}

/* Newest segment activity, from the manifest ffmpeg rewrites as each
 * segment completes.  One stat, whatever shape the recording tree is. */
static long long segment_mtime_ms(const char *path)
{
    struct stat info;

    if (path == NULL || path[0] == '\0' || stat(path, &info) != 0) {
        return 0;
    }
    return (long long)info.st_mtime * 1000;
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
        ssize_t count = read(fd, buffer + total, size - total);

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

static pid_t spawn_child(krtsp_source *source, int *read_fd)
{
    char *argv[KRTSP_ARGV_MAX];
    char storage[KRTSP_ARGV_STORAGE_MAX];
    krtsp_args_request request;
    int fds[2];
    pid_t pid;

    krtsp_args_request_init(&request);
    request.url = source->url;
    request.width = source->options.width;
    request.height = source->options.height;
    request.fps_cap = source->options.fps_cap;
    request.pixfmt = source->options.pixfmt;
    request.low_latency = source->options.low_latency;
    request.letterbox = source->options.letterbox;
    request.legacy_timeout_flag = source->legacy_timeout;
    request.roles = source->roles;
    request.record_dir = source->options.record_dir;
    request.record_pattern = source->options.record_pattern;
    request.segment_seconds = source->options.segment_seconds;
    request.record_audio = source->options.record_audio;
    request.segment_mkdir = source->segment_mkdir;
    request.segment_list = source->segment_list[0] != '\0'
                               ? source->segment_list
                               : NULL;
    /* Each spawn starts its own grace period: a restarted segmenter has
     * legitimately written nothing yet, and carrying the previous run's
     * observation forward would restart-loop it. */
    atomic_store(&source->segment_seen_ms, 0);

    if (krtsp_build_argv(&request, argv, KRTSP_ARGV_MAX, storage,
                         sizeof(storage)) == 0u) {
        return -1;
    }
    argv[0] = source->ffmpeg;

    if (pipe(fds) != 0) {
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        /* Child. */
        (void)close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        (void)close(fds[1]);

        /* stdin from /dev/null, reinforcing -nostdin: otherwise ffmpeg
         * competes with the viewer for terminal input. */
        int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            (void)close(null_fd);
        }

        /* stderr to the log or to /dev/null - never to the terminal,
         * where it would corrupt the alternate screen. */
        int err_fd = -1;
        if (source->options.log_path != NULL) {
            err_fd = open(source->options.log_path,
                          O_WRONLY | O_CREAT | O_APPEND, 0600);
        }
        if (err_fd < 0) {
            err_fd = open("/dev/null", O_WRONLY);
        }
        if (err_fd >= 0) {
            (void)dup2(err_fd, STDERR_FILENO);
            (void)close(err_fd);
        }

        /* Restore default signal handling: a handler inherited from the
         * parent has no meaning in the child. */
        (void)signal(SIGPIPE, SIG_DFL);
        (void)signal(SIGINT, SIG_DFL);
        (void)signal(SIGTERM, SIG_DFL);

        execvp(source->ffmpeg, argv);
        _exit(127);
    }

    /* Parent. */
    (void)close(fds[1]);
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    *read_fd = fds[0];
    return pid;
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
        if (waitpid(pid, NULL, WNOHANG) == pid) {
            return;
        }
        struct timespec pause = {0, 50 * 1000000L};
        (void)nanosleep(&pause, NULL);
        waited_ms += 50;
    }
    (void)kill(pid, SIGKILL);
    (void)waitpid(pid, NULL, 0);
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
        atomic_store(&source->last_frame_ms, monotonic_ms());
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
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&source->lock);
    while (!source->stopping) {
        if (pthread_cond_timedwait(&source->wakeup, &source->lock,
                                   &deadline) == ETIMEDOUT) {
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

        atomic_store(&source->status, (int)KRTSP_STARTING);
        pid = spawn_child(source, &read_fd);
        if (pid < 0) {
            atomic_store(&source->status, (int)KRTSP_OFFLINE);
            atomic_fetch_add(&source->consecutive_failures, 1);
            if (source->options.max_consecutive_failures > 0 &&
                atomic_load(&source->consecutive_failures) >=
                    source->options.max_consecutive_failures) {
                atomic_store(&source->status, (int)KRTSP_FAILED);
                break;
            }
            if (sleep_until_stop(source, backoff)) {
                break;
            }
            backoff = backoff * 2 < source->options.backoff_max_ms
                ? backoff * 2 : source->options.backoff_max_ms;
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

        if (pthread_create(&source->reader, NULL, reader_main, source) != 0) {
            terminate_child(pid);
            (void)close(read_fd);
            pthread_mutex_lock(&source->lock);
            source->child = -1;
            source->pipe_read = -1;
            pthread_mutex_unlock(&source->lock);
            if (sleep_until_stop(source, backoff)) {
                break;
            }
            continue;
        }
        source->reader_started = true;

        /* One-second watch, the interval Frigate settled on: slow enough
         * to be free, fast enough that a wedge is noticed in time. */
        for (;;) {
            long long now;
            int status_value;

            if (sleep_until_stop(source, 1000)) {
                break;
            }
            if (waitpid(pid, &status_value, WNOHANG) == pid) {
                /* Exited on its own; the reader will see EOF. */
                pthread_mutex_lock(&source->lock);
                source->child = -1;
                pthread_mutex_unlock(&source->lock);
                pid = -1;
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
                long long mtime = segment_mtime_ms(source->segment_list);
                int segment_stall = source->options.segment_stall_ms;
                long long seen;

                if (segment_stall <= 0) {
                    int seconds = source->options.segment_seconds > 0
                                      ? source->options.segment_seconds
                                      : 10;
                    /* Three rotations: tolerant of one missed segment,
                     * intolerant of a dead sink. */
                    segment_stall = seconds * 3000;
                }
                if (mtime > source->segment_mtime) {
                    source->segment_mtime = mtime;
                    atomic_store(&source->segment_seen_ms, now);
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
        if (pid > 0) {
            (void)waitpid(pid, NULL, WNOHANG);
        }

        if (is_stopping(source)) {
            break;
        }

        atomic_store(&source->status, (int)KRTSP_OFFLINE);

        /* Reset the backoff only after a sustained online period.  On the
         * first frame instead, a camera that delivers one frame and dies
         * would spin in a tight restart loop forever. */
        if (!stalled &&
            monotonic_ms() - started > source->options.stable_ms) {
            backoff = source->options.backoff_min_ms;
            atomic_store(&source->consecutive_failures, 0);
        } else {
            atomic_fetch_add(&source->consecutive_failures, 1);
        }

        if (source->options.max_consecutive_failures > 0 &&
            atomic_load(&source->consecutive_failures) >=
                source->options.max_consecutive_failures) {
            atomic_store(&source->status, (int)KRTSP_FAILED);
            break;
        }
        if (sleep_until_stop(source, backoff)) {
            break;
        }
        backoff = backoff * 2 < source->options.backoff_max_ms
            ? backoff * 2 : source->options.backoff_max_ms;
    }
    return NULL;
}

/* -------------------------------- public -------------------------------- */

bool krtsp_source_start(
    krtsp_source **out,
    const char *url,
    const krtsp_source_options *options)
{
    krtsp_source_options defaults;
    krtsp_source *source;
    const char *binary;

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
    {
        unsigned roles = options->roles != 0u
                             ? options->roles
                             : (unsigned)KRTSP_ROLE_DECODE;

        if ((roles & ~(unsigned)(KRTSP_ROLE_DECODE | KRTSP_ROLE_RECORD)) !=
            0u) {
            return false;
        }
        /* Frame geometry is only meaningful to the decode sink; a
         * record-only source has no decoded frames to size. */
        if ((roles & (unsigned)KRTSP_ROLE_DECODE) != 0u &&
            (options->width <= 0 || options->height <= 0)) {
            return false;
        }
        if ((roles & (unsigned)KRTSP_ROLE_RECORD) != 0u &&
            (options->record_dir == NULL ||
             options->record_dir[0] == '\0')) {
            return false;
        }
        if (options->segment_seconds < 0 || options->segment_stall_ms < 0) {
            return false;
        }
    }
    if (options->stall_ms <= 0 || options->grace_ms < 0 ||
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
    source->child = -1;
    source->pipe_read = -1;

    binary = options->ffmpeg_path;
    if (binary == NULL || binary[0] == '\0') {
        binary = getenv("KILIX_RTSP_FFMPEG");
    }
    if (binary == NULL || binary[0] == '\0') {
        binary = "ffmpeg";
    }
    if (strlen(binary) >= sizeof(source->ffmpeg)) {
        free(source);
        return false;
    }
    (void)snprintf(source->ffmpeg, sizeof(source->ffmpeg), "%s", binary);
    source->legacy_timeout = krtsp_ffmpeg_needs_legacy_timeout(source->ffmpeg);

    source->roles = options->roles != 0u ? options->roles
                                        : (unsigned)KRTSP_ROLE_DECODE;
    if ((source->roles & (unsigned)KRTSP_ROLE_RECORD) != 0u) {
        /* Probed, not assumed: ffmpeg 5.1 has -strftime but not
         * -strftime_mkdir, and passing a flag the binary lacks fails the
         * spawn outright. */
        source->segment_mkdir =
            krtsp_ffmpeg_supports_segment_mkdir(source->ffmpeg);
        if (snprintf(source->segment_list, sizeof(source->segment_list),
                     "%s/.segments", options->record_dir) < 0) {
            free(source);
            return false;
        }
        atomic_store(&source->record_status, (int)KRTSP_STARTING);
    } else {
        atomic_store(&source->record_status, (int)KRTSP_OFFLINE);
    }

    if ((source->roles & (unsigned)KRTSP_ROLE_DECODE) != 0u) {
        if (!krtsp_frame_init(&source->frame, options->width,
                              options->height)) {
            free(source);
            return false;
        }
        source->frame_size = krtsp_frame_size(source->frame);
    }

    if (pthread_mutex_init(&source->lock, NULL) != 0) {
        krtsp_frame_free(source->frame);
        free(source);
        return false;
    }
    if (pthread_cond_init(&source->wakeup, NULL) != 0) {
        (void)pthread_mutex_destroy(&source->lock);
        krtsp_frame_free(source->frame);
        free(source);
        return false;
    }
    atomic_store(&source->status, (int)KRTSP_STARTING);

    if (pthread_create(&source->supervisor, NULL, supervisor_main,
                       source) != 0) {
        (void)pthread_cond_destroy(&source->wakeup);
        (void)pthread_mutex_destroy(&source->lock);
        krtsp_frame_free(source->frame);
        free(source);
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
    krtsp_frame_free(source->frame);
    free(source);
}

const uint8_t *krtsp_source_borrow(krtsp_source *source, int *age_ms)
{
    if (source == NULL) {
        return NULL;
    }
    return krtsp_frame_borrow(source->frame, NULL, age_ms);
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
