/* How long a detached view lingers before it exits (krtsp_attach.c). */
#include "krtsp_attach.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static long long with(const char *value)
{
    if (value == NULL) {
        (void)unsetenv("KILIX_RTSP_DETACHED_EXIT_SECONDS");
    } else {
        (void)setenv("KILIX_RTSP_DETACHED_EXIT_SECONDS", value, 1);
    }
    return krtsp_attach_detached_exit_ms();
}

int main(void)
{
    CHECK(with(NULL) == 30000);      /* default: 30 s */
    CHECK(with("") == 30000);
    CHECK(with("5") == 5000);
    CHECK(with("0") == 0);           /* 0: wait for a reattach for ever */
    CHECK(with("-1") == 30000);      /* nonsense keeps the default */
    CHECK(with("12x") == 30000);
    CHECK(with("86401") == 30000);
    CHECK(with("86400") == 86400000);
    (void)puts("ok a detached view exits after its grace, and 0 disables it");
    return 0;
}
