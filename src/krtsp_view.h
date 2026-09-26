#ifndef KRTSP_VIEW_H
#define KRTSP_VIEW_H

#include "kilix_rtsp.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * The view command, kept out of the library because it depends on the
 * terminal session and the soft raster, which acquisition does not.
 */

typedef struct krtsp_view_options {
    int fps_cap;
    /* History: how much of the camera to keep behind the live picture.
     * Zero-initialised means KRTSP_BUFFER_AUTO, sized from the disk. */
    krtsp_buffer_spec buffer;
    /* Start with object detection engaged. */
    bool detect;
} krtsp_view_options;

/* Run one camera full-terminal until 'q', escape, or a fatal signal.
 * `label` names the camera in the status banner and is never a URL.
 * `options` may be NULL for the defaults. */
int krtsp_view_run(const char *url, const char *label,
                   const krtsp_view_options *options);

/* Run several cameras in a grid until 'q', escape, or a fatal signal.
 * Each source is decoded directly at its tile size, which is why a
 * mosaic costs less than one full-screen camera. */
int krtsp_mosaic_run(
    const char **urls, const char **labels, size_t count, int fps_cap);

#endif
