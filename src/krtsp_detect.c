#include "krtsp_detect.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* -------------------------------- classes ------------------------------- */

static const char *const COCO[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train",
    "truck", "boat", "traffic light", "fire hydrant", "stop sign",
    "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag",
    "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball", "kite",
    "baseball bat", "baseball glove", "skateboard", "surfboard",
    "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon",
    "bowl", "banana", "apple", "sandwich", "orange", "broccoli", "carrot",
    "hot dog", "pizza", "donut", "cake", "chair", "couch", "potted plant",
    "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote",
    "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush"
};

const char *krtsp_detect_class_name(int class_id)
{
    if (class_id < 0 ||
        (size_t)class_id >= sizeof(COCO) / sizeof(COCO[0])) {
        return "object";
    }
    return COCO[class_id];
}

/* ------------------------------ geometry -------------------------------- */

krtsp_fit krtsp_detect_fit(int frame_width, int frame_height, int size)
{
    krtsp_fit fit = {1.0f, 0, 0, size, size};
    float across;
    float down;

    if (frame_width <= 0 || frame_height <= 0 || size <= 0) {
        return fit;
    }
    across = (float)size / (float)frame_width;
    down = (float)size / (float)frame_height;
    fit.scale = across < down ? across : down;
    fit.width = (int)((float)frame_width * fit.scale + 0.5f);
    fit.height = (int)((float)frame_height * fit.scale + 0.5f);
    if (fit.width < 1) {
        fit.width = 1;
    }
    if (fit.height < 1) {
        fit.height = 1;
    }
    if (fit.width > size) {
        fit.width = size;
    }
    if (fit.height > size) {
        fit.height = size;
    }
    fit.pad_x = (size - fit.width) / 2;
    fit.pad_y = (size - fit.height) / 2;
    return fit;
}

void krtsp_detect_square(
    const uint8_t *rgba, int frame_width, int frame_height, int size,
    uint8_t *bgra)
{
    krtsp_fit fit;

    if (rgba == NULL || bgra == NULL || frame_width <= 0 ||
        frame_height <= 0 || size <= 0) {
        return;
    }
    fit = krtsp_detect_fit(frame_width, frame_height, size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint8_t *out = bgra + ((size_t)y * (size_t)size + (size_t)x) * 4u;
            unsigned red = 0u;
            unsigned green = 0u;
            unsigned blue = 0u;
            unsigned count = 0u;
            int x0;
            int x1;
            int y0;
            int y1;

            if (x < fit.pad_x || x >= fit.pad_x + fit.width ||
                y < fit.pad_y || y >= fit.pad_y + fit.height) {
                out[0] = 0u;
                out[1] = 0u;
                out[2] = 0u;
                out[3] = 255u;
                continue;
            }
            /* The source rectangle this pixel covers, at least one pixel
             * wide so an upscaled frame still reads its nearest neighbour. */
            x0 = (int)((float)(x - fit.pad_x) / fit.scale);
            x1 = (int)((float)(x + 1 - fit.pad_x) / fit.scale + 0.999f);
            y0 = (int)((float)(y - fit.pad_y) / fit.scale);
            y1 = (int)((float)(y + 1 - fit.pad_y) / fit.scale + 0.999f);
            if (x1 <= x0) {
                x1 = x0 + 1;
            }
            if (y1 <= y0) {
                y1 = y0 + 1;
            }
            if (x1 > frame_width) {
                x1 = frame_width;
            }
            if (y1 > frame_height) {
                y1 = frame_height;
            }
            if (x0 >= x1) {
                x0 = x1 - 1;
            }
            if (y0 >= y1) {
                y0 = y1 - 1;
            }
            for (int sy = y0; sy < y1; ++sy) {
                const uint8_t *row = rgba + (size_t)sy * (size_t)frame_width * 4u;

                for (int sx = x0; sx < x1; ++sx) {
                    red += row[(size_t)sx * 4u];
                    green += row[(size_t)sx * 4u + 1u];
                    blue += row[(size_t)sx * 4u + 2u];
                    ++count;
                }
            }
            out[0] = (uint8_t)(blue / count);
            out[1] = (uint8_t)(green / count);
            out[2] = (uint8_t)(red / count);
            out[3] = 255u;
        }
    }
}

static int clip(float value, int low, int high)
{
    int rounded = (int)lroundf(value);

    return rounded < low ? low : rounded > high ? high : rounded;
}

size_t krtsp_detect_parse(
    const float *reply, int frame_width, int frame_height, int size,
    float min_score, krtsp_detection *out, size_t capacity)
{
    krtsp_fit fit;
    size_t stored = 0u;

    if (reply == NULL || out == NULL || capacity == 0u || frame_width <= 0 ||
        frame_height <= 0 || size <= 0) {
        return 0u;
    }
    fit = krtsp_detect_fit(frame_width, frame_height, size);
    for (int row = 0; row < KRTSP_DETECT_ROWS && stored < capacity; ++row) {
        const float *at = reply + (size_t)row * KRTSP_DETECT_COLUMNS;
        krtsp_detection box;
        float x0;
        float y0;
        float x1;
        float y1;

        if (!isfinite(at[0]) || !isfinite(at[1]) || !isfinite(at[2]) ||
            !isfinite(at[3]) || !isfinite(at[4]) || !isfinite(at[5]) ||
            at[0] < 0.0f || at[0] > 1000.0f || at[1] < min_score) {
            continue;
        }
        /* [class, score, y0, x0, y1, x1], normalised to the square; the
         * frame occupies only the fitted part of it. */
        y0 = (at[2] * (float)size - (float)fit.pad_y) / fit.scale;
        x0 = (at[3] * (float)size - (float)fit.pad_x) / fit.scale;
        y1 = (at[4] * (float)size - (float)fit.pad_y) / fit.scale;
        x1 = (at[5] * (float)size - (float)fit.pad_x) / fit.scale;
        box.class_id = (int)at[0];
        box.score = at[1];
        box.x0 = clip(x0, 0, frame_width);
        box.y0 = clip(y0, 0, frame_height);
        box.x1 = clip(x1, 0, frame_width);
        box.y1 = clip(y1, 0, frame_height);
        if (box.x1 <= box.x0 || box.y1 <= box.y0) {
            continue;
        }
        out[stored++] = box;
    }
    return stored;
}

/* ------------------------------ resolution ------------------------------ */

static bool executable(const char *path)
{
    return path != NULL && access(path, X_OK) == 0;
}

size_t krtsp_detect_resolve(
    char *storage, size_t storage_size, const char **argv, size_t capacity)
{
    const char *variable = getenv("KILIX_OBJECT_DETECTOR");
    const char *data = getenv("GPU_TERMINAL_DATA_HOME");
    const char *home = getenv("HOME");
    char base[PATH_MAX];
    size_t used = 0u;
    size_t words = 0u;

    if (storage == NULL || storage_size == 0u || argv == NULL ||
        capacity < 2u) {
        return 0u;
    }
    if (variable != NULL && variable[0] != '\0') {
        /* Split on spaces with no quoting, exactly as the rest of the
         * stack reads this variable, so one value means one command. */
        size_t length = strlen(variable);

        if (length >= storage_size) {
            return 0u;   /* refused whole rather than run truncated */
        }
        (void)memcpy(storage, variable, length + 1u);
        for (char *at = storage; *at != '\0';) {
            while (*at == ' ') {
                *at++ = '\0';
            }
            if (*at == '\0') {
                break;
            }
            if (words + 1u >= capacity) {
                return 0u;
            }
            argv[words++] = at;
            while (*at != '\0' && *at != ' ') {
                ++at;
            }
        }
        argv[words] = NULL;
        /* A variable naming a path that is not there - left behind by a
         * moved home directory, or a runtime since removed - is stale, not
         * a choice: fall through to what is installed rather than fail to
         * start a detector that is sitting right there. */
        if (words > 0u && strchr(argv[0], '/') != NULL &&
            !executable(argv[0])) {
            words = 0u;
        } else {
            return words;
        }
    }

    if (data != NULL && data[0] != '\0') {
        (void)snprintf(base, sizeof(base), "%s", data);
    } else if (home != NULL && home[0] != '\0') {
        (void)snprintf(base, sizeof(base), "%s/.local/gpu_terminal", home);
    } else {
        return 0u;
    }
    {
        static const char *const runtimes[] = {
            "runtimes/yolox/bin/kilix-yolox-detect",
            "runtimes/yolo/bin/kilix-look-detect"
        };

        for (size_t index = 0u; index < 2u; ++index) {
            int printed = snprintf(storage + used, storage_size - used,
                                   "%s/%s", base, runtimes[index]);

            if (printed < 0 || (size_t)printed >= storage_size - used) {
                return 0u;
            }
            if (executable(storage + used)) {
                argv[0] = storage + used;
                argv[1] = NULL;
                return 1u;
            }
        }
    }
    return 0u;
}

/* ------------------------------- detector ------------------------------- */

#define WARMUP_MS 90000
#define REPLY_MS 5000
#define SLICE_MS 250

struct krtsp_detector {
    pid_t pid;
    int to_child;
    int from_child;
    pthread_t thread;
    bool thread_started;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    bool stop;
    bool pending;
    bool busy;
    bool warm;
    krtsp_detector_state state;
    char error[128];
    uint8_t *frame;
    size_t frame_capacity;
    int frame_width;
    int frame_height;
    krtsp_detection boxes[KRTSP_DETECT_MAX];
    size_t box_count;
    uint64_t generation;
    uint8_t square[KRTSP_DETECT_SIZE * KRTSP_DETECT_SIZE * 4];
};

static void fail(krtsp_detector *detector, const char *why)
{
    (void)pthread_mutex_lock(&detector->lock);
    detector->state = KRTSP_DETECTOR_FAILED;
    (void)snprintf(detector->error, sizeof(detector->error), "%s", why);
    detector->box_count = 0u;
    (void)pthread_mutex_unlock(&detector->lock);
}

static bool write_all(int fd, const uint8_t *bytes, size_t size)
{
    while (size > 0u) {
        ssize_t written = write(fd, bytes, size);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        bytes += written;
        size -= (size_t)written;
    }
    return true;
}

/* One whole reply, or a reason.  Sliced so a stop request is heard within
 * a quarter second even while a model is loading for a minute. */
static const char *read_reply(krtsp_detector *detector, uint8_t *reply)
{
    size_t have = 0u;
    int budget = detector->warm ? REPLY_MS : WARMUP_MS;

    while (have < KRTSP_DETECT_REPLY_BYTES) {
        struct pollfd wait = {detector->from_child, POLLIN, 0};
        int ready;
        bool stopping;

        (void)pthread_mutex_lock(&detector->lock);
        stopping = detector->stop;
        (void)pthread_mutex_unlock(&detector->lock);
        if (stopping) {
            return "stopped";
        }
        ready = poll(&wait, 1, SLICE_MS);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return "cannot read the detector";
        }
        if (ready == 0) {
            budget -= SLICE_MS;
            if (budget <= 0) {
                return detector->warm ? "the detector stopped answering"
                                      : "the detector never finished loading";
            }
            continue;
        }
        {
            ssize_t got = read(detector->from_child, reply + have,
                               KRTSP_DETECT_REPLY_BYTES - have);

            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return "cannot read the detector";
            }
            if (got == 0) {
                return "the detector exited";
            }
            have += (size_t)got;
        }
    }
    return NULL;
}

static void *worker(void *argument)
{
    krtsp_detector *detector = argument;
    float reply[KRTSP_DETECT_ROWS * KRTSP_DETECT_COLUMNS];
    sigset_t block;

    /* A detector that dies mid-write must cost this thread an EPIPE, not
     * the process a SIGPIPE. */
    (void)sigemptyset(&block);
    (void)sigaddset(&block, SIGPIPE);
    (void)pthread_sigmask(SIG_BLOCK, &block, NULL);

    for (;;) {
        int width;
        int height;
        const char *problem;

        (void)pthread_mutex_lock(&detector->lock);
        while (!detector->pending && !detector->stop) {
            (void)pthread_cond_wait(&detector->wake, &detector->lock);
        }
        if (detector->stop) {
            (void)pthread_mutex_unlock(&detector->lock);
            return NULL;
        }
        detector->pending = false;
        detector->busy = true;
        width = detector->frame_width;
        height = detector->frame_height;
        (void)pthread_mutex_unlock(&detector->lock);

        krtsp_detect_square(detector->frame, width, height, KRTSP_DETECT_SIZE,
                            detector->square);
        if (!write_all(detector->to_child, detector->square,
                       sizeof(detector->square))) {
            fail(detector, "cannot write to the detector");
            return NULL;
        }
        problem = read_reply(detector, (uint8_t *)(void *)reply);
        if (problem != NULL) {
            if (strcmp(problem, "stopped") != 0) {
                fail(detector, problem);
            }
            return NULL;
        }

        (void)pthread_mutex_lock(&detector->lock);
        detector->warm = true;
        detector->state = KRTSP_DETECTOR_RUNNING;
        detector->box_count = krtsp_detect_parse(
            reply, width, height, KRTSP_DETECT_SIZE, KRTSP_DETECT_MIN_SCORE,
            detector->boxes, KRTSP_DETECT_MAX);
        ++detector->generation;
        detector->busy = false;
        (void)pthread_mutex_unlock(&detector->lock);
    }
}

/* pipe2() is a GNU extension; a pipe with close-on-exec set afterwards is
 * portable, and the window between the two calls matters only to a thread
 * spawning at the same instant, which nothing here does. */
static bool make_pipe(int fds[2])
{
    if (pipe(fds) != 0) {
        return false;
    }
    for (int index = 0; index < 2; ++index) {
        int flags = fcntl(fds[index], F_GETFD);

        if (flags < 0 || fcntl(fds[index], F_SETFD, flags | FD_CLOEXEC) != 0) {
            (void)close(fds[0]);
            (void)close(fds[1]);
            fds[0] = -1;
            fds[1] = -1;
            return false;
        }
    }
    return true;
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

bool krtsp_detector_start(krtsp_detector **detector, const char *const *argv)
{
    krtsp_detector *made;
    const char *words[16];
    char geometry[32];
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    size_t count = 0u;
    int spawned;

    if (detector == NULL || argv == NULL || argv[0] == NULL) {
        return false;
    }
    *detector = NULL;
    for (; argv[count] != NULL; ++count) {
        if (count + 3u >= sizeof(words) / sizeof(words[0])) {
            return false;
        }
        words[count] = argv[count];
    }
    (void)snprintf(geometry, sizeof(geometry), "%dx%d", KRTSP_DETECT_SIZE,
                   KRTSP_DETECT_SIZE);
    words[count++] = "--geometry";
    words[count++] = geometry;
    words[count] = NULL;

    made = calloc(1u, sizeof(*made));
    if (made == NULL) {
        return false;
    }
    made->to_child = -1;
    made->from_child = -1;
    (void)pthread_mutex_init(&made->lock, NULL);
    (void)pthread_cond_init(&made->wake, NULL);

    if (!make_pipe(in_pipe) || !make_pipe(out_pipe) ||
        posix_spawn_file_actions_init(&actions) != 0) {
        close_fd(&in_pipe[0]);
        close_fd(&in_pipe[1]);
        close_fd(&out_pipe[0]);
        close_fd(&out_pipe[1]);
        free(made);
        return false;
    }
    /* Its stdin and stdout are the pipes; its stderr is nobody's - an
     * inference library's warnings written into the alternate screen
     * corrupt the picture. */
    if (posix_spawn_file_actions_adddup2(&actions, in_pipe[0],
                                         STDIN_FILENO) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, out_pipe[1],
                                         STDOUT_FILENO) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                         O_WRONLY, 0) != 0) {
        (void)posix_spawn_file_actions_destroy(&actions);
        close_fd(&in_pipe[0]);
        close_fd(&in_pipe[1]);
        close_fd(&out_pipe[0]);
        close_fd(&out_pipe[1]);
        free(made);
        return false;
    }
    spawned = posix_spawnp(&made->pid, words[0], &actions, NULL,
                           (char *const *)(void *)words, environ);
    (void)posix_spawn_file_actions_destroy(&actions);
    close_fd(&in_pipe[0]);
    close_fd(&out_pipe[1]);
    if (spawned != 0) {
        close_fd(&in_pipe[1]);
        close_fd(&out_pipe[0]);
        free(made);
        return false;
    }
    made->to_child = in_pipe[1];
    made->from_child = out_pipe[0];
    made->state = KRTSP_DETECTOR_LOADING;

    if (pthread_create(&made->thread, NULL, worker, made) != 0) {
        krtsp_detector_stop(made);
        return false;
    }
    made->thread_started = true;
    *detector = made;
    return true;
}

void krtsp_detector_stop(krtsp_detector *detector)
{
    int status;

    if (detector == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&detector->lock);
    detector->stop = true;
    (void)pthread_cond_broadcast(&detector->wake);
    (void)pthread_mutex_unlock(&detector->lock);
    /* The child goes first: a worker blocked writing a frame to a model
     * that is still loading only wakes when the other end closes. */
    if (detector->pid > 0) {
        (void)kill(detector->pid, SIGTERM);
    }
    if (detector->thread_started) {
        (void)pthread_join(detector->thread, NULL);
    }
    close_fd(&detector->to_child);
    close_fd(&detector->from_child);
    if (detector->pid > 0) {
        for (int wait = 0; wait < 20; ++wait) {
            if (waitpid(detector->pid, &status, WNOHANG) != 0) {
                detector->pid = 0;
                break;
            }
            {
                struct timespec pause = {0, 50 * 1000 * 1000};

                (void)nanosleep(&pause, NULL);
            }
        }
        if (detector->pid > 0) {
            (void)kill(detector->pid, SIGKILL);
            (void)waitpid(detector->pid, &status, 0);
        }
    }
    (void)pthread_mutex_destroy(&detector->lock);
    (void)pthread_cond_destroy(&detector->wake);
    free(detector->frame);
    free(detector);
}

bool krtsp_detector_submit(
    krtsp_detector *detector, const uint8_t *rgba, int width, int height)
{
    size_t bytes;
    bool accepted = false;

    if (detector == NULL || rgba == NULL || width <= 0 || height <= 0 ||
        (size_t)width > SIZE_MAX / 4u / (size_t)height) {
        return false;
    }
    bytes = (size_t)width * (size_t)height * 4u;
    (void)pthread_mutex_lock(&detector->lock);
    if (!detector->pending && !detector->busy && !detector->stop &&
        detector->state != KRTSP_DETECTOR_FAILED) {
        if (bytes > detector->frame_capacity) {
            uint8_t *grown = realloc(detector->frame, bytes);

            if (grown != NULL) {
                detector->frame = grown;
                detector->frame_capacity = bytes;
            }
        }
        if (bytes <= detector->frame_capacity) {
            (void)memcpy(detector->frame, rgba, bytes);
            detector->frame_width = width;
            detector->frame_height = height;
            detector->pending = true;
            accepted = true;
            (void)pthread_cond_signal(&detector->wake);
        }
    }
    (void)pthread_mutex_unlock(&detector->lock);
    return accepted;
}

size_t krtsp_detector_take(
    krtsp_detector *detector, krtsp_detection *out, size_t capacity,
    uint64_t *generation)
{
    size_t count;

    if (detector == NULL || out == NULL) {
        return 0u;
    }
    (void)pthread_mutex_lock(&detector->lock);
    count = detector->box_count < capacity ? detector->box_count : capacity;
    (void)memcpy(out, detector->boxes, count * sizeof(*out));
    if (generation != NULL) {
        *generation = detector->generation;
    }
    (void)pthread_mutex_unlock(&detector->lock);
    return count;
}

krtsp_detector_state krtsp_detector_status(const krtsp_detector *detector)
{
    krtsp_detector_state state;

    if (detector == NULL) {
        return KRTSP_DETECTOR_FAILED;
    }
    (void)pthread_mutex_lock((pthread_mutex_t *)(void *)&detector->lock);
    state = detector->state;
    (void)pthread_mutex_unlock((pthread_mutex_t *)(void *)&detector->lock);
    return state;
}

const char *krtsp_detector_error(const krtsp_detector *detector)
{
    return detector == NULL ? "" : detector->error;
}
