#ifndef KRTSP_VIEW_H
#define KRTSP_VIEW_H

/*
 * The view command, kept out of the library because it depends on the
 * terminal session and the soft raster, which acquisition does not.
 */

/* Run one camera full-terminal until 'q', escape, or a fatal signal.
 * `label` names the camera in the status banner and is never a URL. */
int krtsp_view_run(const char *url, const char *label, int fps_cap);

#endif
