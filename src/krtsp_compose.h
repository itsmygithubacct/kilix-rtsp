#ifndef KRTSP_COMPOSE_H
#define KRTSP_COMPOSE_H

/*
 * The mosaic composite: tile frames, captions, and the pack to RGBA.
 *
 * Kept out of the terminal loop so the exact pixels a mosaic produces
 * can be tested and measured without a camera or a terminal.  The loop
 * in krtsp_view.c decides *when* to compose; this decides *what* the
 * composed frame contains, and nothing here touches a session.
 *
 * Like the view command itself this depends on the soft raster, so it
 * stays out of the acquisition library.
 */

#include "kilix_rtsp.h"
#include "soft_raster.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KRTSP_COMPOSE_MAX 16

/* What one tile shows in this composite. */
typedef struct krtsp_compose_tile {
    krtsp_tile tile;        /* placement inside the canvas */
    const uint8_t *pixels;  /* BGRA rows at exactly tile size; NULL when
                             * no frame has arrived */
    uint64_t sequence;      /* frame counter; an equal sequence promises
                             * identical pixels */
    const char *caption;    /* drawn over the top of the tile, never NULL */
    uint32_t accent;        /* status color for the caption bar */
} krtsp_compose_tile;

typedef struct krtsp_compositor {
    uint32_t *pixels;       /* canvas storage, 0xAARRGGBB */
    int width;
    int height;
} krtsp_compositor;

bool krtsp_compositor_init(krtsp_compositor *compositor, int width,
                           int height);
void krtsp_compositor_free(krtsp_compositor *compositor);

/*
 * Compose `count` tiles and pack the result into rgba_out, which must
 * hold width * height * 4 bytes.  Tiles must lie inside the canvas and
 * not overlap - krtsp_mosaic_layout() guarantees both.
 */
bool krtsp_compose(krtsp_compositor *compositor,
                   const krtsp_compose_tile *tiles, size_t count,
                   uint8_t *rgba_out, size_t rgba_capacity);

#endif
