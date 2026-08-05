#ifndef KRTSP_ATTACH_H
#define KRTSP_ATTACH_H

/*
 * Detects whether a kilix pane still has a frontend attached, so a view
 * that nobody can see can stop decoding.  See krtsp_attach.c for why this
 * cannot be inferred from the terminal itself.
 */

#include <stdbool.h>

#define KRTSP_ATTACH_ID_MAX 128
#define KRTSP_ATTACH_DIR_MAX 512

typedef struct krtsp_attach {
    bool available;                        /* running under a broker */
    char session_id[KRTSP_ATTACH_ID_MAX];
    char runtime_dir[KRTSP_ATTACH_DIR_MAX];
} krtsp_attach;

void krtsp_attach_init(krtsp_attach *watch);

/* True when a frontend is attached, or when there is no broker to ask. */
bool krtsp_attach_is_attached(krtsp_attach *watch);

#endif
