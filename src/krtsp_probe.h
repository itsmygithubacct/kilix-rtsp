#ifndef KRTSP_PROBE_H
#define KRTSP_PROBE_H

#include <stdbool.h>
#include <stddef.h>

/* Internal ffprobe front end.  Result codes are the child exit status or one
 * of KRTSP_EXEC_* from krtsp_exec.h. */
int krtsp_run_ffprobe(const char *url, bool force_tcp, char *output,
                      size_t capacity, int timeout_ms)
    __attribute__((visibility("hidden")));

#endif
