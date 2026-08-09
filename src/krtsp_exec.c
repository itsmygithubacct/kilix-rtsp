/* Bounded, shell-free subprocess output capture shared by probes. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "krtsp_exec.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static int64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
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

/* File actions are much easier to reason about when neither pipe end aliases
 * stdin/stdout/stderr. */
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

static bool spawn_capture(const char *path, char *const argv[], int fds[2],
                          pid_t *pid)
{
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    sigset_t defaults;
    sigset_t mask;
    short flags = 0;
    int rc;

    if (posix_spawn_file_actions_init(&actions) != 0) {
        return false;
    }
    if (posix_spawnattr_init(&attributes) != 0) {
        (void)posix_spawn_file_actions_destroy(&actions);
        return false;
    }
    if (sigemptyset(&defaults) != 0 || sigaddset(&defaults, SIGPIPE) != 0 ||
        sigaddset(&defaults, SIGHUP) != 0 ||
        sigaddset(&defaults, SIGINT) != 0 ||
        sigaddset(&defaults, SIGQUIT) != 0 ||
        sigaddset(&defaults, SIGTERM) != 0 || sigemptyset(&mask) != 0 ||
        posix_spawnattr_setsigdefault(&attributes, &defaults) != 0 ||
        posix_spawnattr_setsigmask(&attributes, &mask) != 0) {
        (void)posix_spawnattr_destroy(&attributes);
        (void)posix_spawn_file_actions_destroy(&actions);
        return false;
    }
    flags = (short)(POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    if (posix_spawnattr_setflags(&attributes, flags) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO) != 0 ||
        posix_spawn_file_actions_addclose(&actions, fds[0]) != 0 ||
        posix_spawn_file_actions_addclose(&actions, fds[1]) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                         O_RDONLY, 0) != 0 ||
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                         O_WRONLY, 0) != 0) {
        (void)posix_spawnattr_destroy(&attributes);
        (void)posix_spawn_file_actions_destroy(&actions);
        return false;
    }

    rc = posix_spawnp(pid, path, &actions, &attributes, argv, environ);
    (void)posix_spawnattr_destroy(&attributes);
    (void)posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        errno = rc;
        return false;
    }
    return true;
}

static void reap_after_kill(pid_t pid)
{
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
}

int krtsp_exec_capture(const char *path, char *const argv[], char *output,
                       size_t capacity, int timeout_ms)
{
    int fds[2] = {-1, -1};
    pid_t pid;
    int64_t deadline;
    size_t used = 0u;
    bool truncated = false;
    bool pipe_done = false;
    bool child_done = false;
    int status = 0;

    if (output != NULL && capacity > 0u) {
        output[0] = '\0';
    }
    if (path == NULL || path[0] == '\0' || argv == NULL || argv[0] == NULL ||
        output == NULL || capacity == 0u || timeout_ms <= 0) {
        errno = EINVAL;
        return KRTSP_EXEC_ERROR;
    }
    deadline = monotonic_ms();
    if (deadline < 0 || deadline > INT64_MAX - timeout_ms) {
        return KRTSP_EXEC_ERROR;
    }
    deadline += timeout_ms;

    if (!pipe_cloexec(fds) || !move_above_stdio(&fds[0]) ||
        !move_above_stdio(&fds[1])) {
        int saved = errno;

        if (fds[0] >= 0) {
            (void)close(fds[0]);
        }
        if (fds[1] >= 0) {
            (void)close(fds[1]);
        }
        errno = saved;
        return KRTSP_EXEC_ERROR;
    }
    if (!spawn_capture(path, argv, fds, &pid)) {
        int saved = errno;

        (void)close(fds[0]);
        (void)close(fds[1]);
        errno = saved;
        return KRTSP_EXEC_ERROR;
    }
    (void)close(fds[1]);
    fds[1] = -1;

    while (!pipe_done || !child_done) {
        int64_t now = monotonic_ms();
        int wait_ms;
        struct pollfd descriptor;

        if (now < 0 || now >= deadline) {
            if (!child_done) {
                (void)kill(pid, SIGKILL);
                reap_after_kill(pid);
            }
            (void)close(fds[0]);
            return KRTSP_EXEC_TIMEOUT;
        }
        wait_ms = (int)(deadline - now);
        if (wait_ms > 50) {
            wait_ms = 50;
        }
        descriptor.fd = fds[0];
        descriptor.events = (short)(POLLIN | POLLHUP);
        descriptor.revents = 0;
        if (!pipe_done) {
            int polled;

            do {
                polled = poll(&descriptor, 1u, wait_ms);
            } while (polled < 0 && errno == EINTR);
            if (polled < 0) {
                (void)kill(pid, SIGKILL);
                reap_after_kill(pid);
                (void)close(fds[0]);
                return KRTSP_EXEC_ERROR;
            }
            if (polled > 0 &&
                (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                char discard[4096];
                char *destination = used + 1u < capacity ? output + used
                                                         : discard;
                size_t available = used + 1u < capacity
                                       ? capacity - used - 1u
                                       : sizeof(discard);
                ssize_t got;

                do {
                    got = read(fds[0], destination, available);
                } while (got < 0 && errno == EINTR);
                if (got > 0) {
                    if (destination == discard) {
                        truncated = true;
                    } else {
                        used += (size_t)got;
                        output[used] = '\0';
                    }
                } else if (got == 0) {
                    pipe_done = true;
                } else {
                    (void)kill(pid, SIGKILL);
                    reap_after_kill(pid);
                    (void)close(fds[0]);
                    return KRTSP_EXEC_ERROR;
                }
            }
        } else if (!child_done) {
            /* A program may deliberately close stdout before it exits.  Keep
             * the deadline without turning that interval into a busy loop. */
            (void)poll(NULL, 0u, wait_ms);
        }
        if (!child_done) {
            pid_t waited;

            do {
                waited = waitpid(pid, &status, WNOHANG);
            } while (waited < 0 && errno == EINTR);
            if (waited == pid) {
                child_done = true;
            } else if (waited < 0) {
                (void)kill(pid, SIGKILL);
                reap_after_kill(pid);
                (void)close(fds[0]);
                return KRTSP_EXEC_ERROR;
            }
        }
        /* Once the child is gone, a final HUP/read iteration drains the
         * bytes already buffered in the pipe. */
    }
    (void)close(fds[0]);
    if (truncated) {
        return KRTSP_EXEC_TRUNCATED;
    }
    if (!WIFEXITED(status)) {
        return KRTSP_EXEC_ERROR;
    }
    return WEXITSTATUS(status);
}
