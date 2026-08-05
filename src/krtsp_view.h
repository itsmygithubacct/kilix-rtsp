#ifndef KRTSP_VIEW_H
#define KRTSP_VIEW_H

#include <stddef.h>

/*
 * The view command, kept out of the library because it depends on the
 * terminal session and the soft raster, which acquisition does not.
 */

/* Run one camera full-terminal until 'q', escape, or a fatal signal.
 * `label` names the camera in the status banner and is never a URL. */
int krtsp_view_run(const char *url, const char *label, int fps_cap);

/* Run several cameras in a grid until 'q', escape, or a fatal signal.
 * Each source is decoded directly at its tile size, which is why a
 * mosaic costs less than one full-screen camera. */
int krtsp_mosaic_run(
    const char **urls, const char **labels, size_t count, int fps_cap);

#endif
