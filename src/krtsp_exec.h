#ifndef KRTSP_EXEC_H
#define KRTSP_EXEC_H

#include <stddef.h>

/* Internal subprocess result codes.  Non-negative values are ordinary child
 * exit statuses. */
#define KRTSP_EXEC_ERROR (-1)
#define KRTSP_EXEC_TIMEOUT (-2)
#define KRTSP_EXEC_TRUNCATED (-3)

/*
 * Run `path` directly (never through a shell), capture stdout, and enforce a
 * monotonic wall-clock deadline.  stdin and stderr are redirected to
 * /dev/null.  Output is always NUL-terminated when capacity is nonzero.
 */
int krtsp_exec_capture(const char *path, char *const argv[], char *output,
                       size_t capacity, int timeout_ms)
    __attribute__((visibility("hidden")));

#endif
