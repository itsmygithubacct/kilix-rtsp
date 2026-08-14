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
 * The canvas persists between composites, so a composite redraws only
 * the tiles whose frame, caption, or accent changed since the last one
 * - the margins and the unchanged tiles are already right - and reports
 * what it touched as damage rectangles, which is exactly what a
 * damage-aware present wants to hear.  The output buffer always holds
 * the complete frame regardless; the rectangles only say which parts
 * are new.  A caption too wide for its tile spills past it, and a
 * shorter successor would strand the overhang, so that one case falls
 * back to a full rebuild rather than reasoning about what the spill
 * touched.
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
#define KRTSP_COMPOSE_CAPTION_MAX 128

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

/* A region of the output that changed; x1/y1 exclusive. */
typedef struct krtsp_damage {
    int x0;
    int y0;
    int x1;
    int y1;
} krtsp_damage;

/* What the compositor remembers about a tile between composites. */
typedef struct krtsp_compose_slot {
    krtsp_tile tile;
    uint64_t sequence;
    uint32_t accent;
    bool has_pixels;
    bool caption_overlong;  /* longer than stored: always redrawn */
    char caption[KRTSP_COMPOSE_CAPTION_MAX];
} krtsp_compose_slot;

typedef struct krtsp_compositor {
    uint32_t *pixels;       /* canvas storage, 0xAARRGGBB */
    int width;
    int height;
    bool fresh;             /* nothing composed since init/reset */
    size_t count;           /* slots in use by the last composite */
    krtsp_compose_slot slots[KRTSP_COMPOSE_MAX];
} krtsp_compositor;

bool krtsp_compositor_init(krtsp_compositor *compositor, int width,
                           int height);

/* Forget everything composed so far: the next composite rebuilds and
 * reports the whole canvas as damage.  For when the screen's content
 * can no longer be assumed, e.g. sources were restarted. */
void krtsp_compositor_reset(krtsp_compositor *compositor);

void krtsp_compositor_free(krtsp_compositor *compositor);

/*
 * Compose `count` tiles and pack the complete frame into rgba_out,
 * which must hold width * height * 4 bytes.  Tiles must lie inside the
 * canvas and not overlap - krtsp_mosaic_layout() guarantees both.
 *
 * When `damage` is non-NULL, up to damage_capacity rectangles covering
 * every pixel that may differ from the previous composite are stored
 * there and their number in *damage_count; if they would not fit, one
 * rectangle covering the whole canvas is stored instead.  Zero
 * rectangles is the honest answer when nothing changed.
 */
bool krtsp_compose(krtsp_compositor *compositor,
                   const krtsp_compose_tile *tiles, size_t count,
                   uint8_t *rgba_out, size_t rgba_capacity,
                   krtsp_damage *damage, size_t damage_capacity,
                   size_t *damage_count);

#endif
