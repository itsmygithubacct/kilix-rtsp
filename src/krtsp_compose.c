/*
 * See krtsp_compose.h.  The drawing here defines the mosaic's pixels:
 * a dark canvas, one blit per tile (or a placeholder fill when a camera
 * has produced nothing), and a captioned band across each tile's top -
 * a translucent plate, a status-colored bar, and the label text.
 */

#include "krtsp_compose.h"

#include <stdlib.h>
#include <string.h>

#define CANVAS_CLEAR 0x0A0A0Cu
#define TILE_EMPTY   0x141418u

static bool canvas_size(int width, int height, size_t *bytes)
{
    if (bytes == NULL || width <= 0 || height <= 0 ||
        (size_t)width > SIZE_MAX / 4u / (size_t)height) {
        return false;
    }
    *bytes = (size_t)width * (size_t)height * 4u;
    return true;
}

bool krtsp_compositor_init(krtsp_compositor *compositor, int width,
                           int height)
{
    size_t bytes;

    if (compositor == NULL || !canvas_size(width, height, &bytes)) {
        return false;
    }
    compositor->pixels = malloc(bytes);
    if (compositor->pixels == NULL) {
        return false;
    }
    compositor->width = width;
    compositor->height = height;
    return true;
}

void krtsp_compositor_free(krtsp_compositor *compositor)
{
    if (compositor == NULL) {
        return;
    }
    free(compositor->pixels);
    compositor->pixels = NULL;
    compositor->width = 0;
    compositor->height = 0;
}

/* Every tile is captioned.  In a grid, "which camera is that" is the
 * first question, and an unlabelled tile that has frozen is
 * indistinguishable from a quiet scene. */
static void draw_caption(sr_canvas *canvas, const krtsp_compose_tile *entry)
{
    const krtsp_tile *tile = &entry->tile;
    int pad = 4;
    int band = SR_FONT_H + pad;

    /* A translucent plate keeps the text readable over a bright scene
     * without hiding the top of the picture.  For an integer-aligned
     * rect the fill's edge coverage is exactly 1, so this lays down the
     * same pixels a per-pixel blend loop would - test_compose holds it
     * to that - without a call and a clip check per pixel. */
    sr_fill_rect(canvas, (float)tile->x, (float)tile->y,
                 (float)tile->width, (float)band, 0x000000u, 0.5f);
    sr_fill_rect(canvas, (float)tile->x, (float)tile->y, 3.0f, (float)band,
                 entry->accent, 1.0f);
    sr_text_shadow(canvas, (float)(tile->x + pad + 3),
                   (float)(tile->y + pad / 2), entry->caption,
                   0xFFFFFFu, 1.0f, 1);
}

static void draw_tile(sr_canvas *canvas, const krtsp_compose_tile *entry)
{
    const krtsp_tile *tile = &entry->tile;

    if (entry->pixels != NULL) {
        sr_canvas source_canvas;

        sr_canvas_wrap(&source_canvas,
                       (uint32_t *)(void *)(uintptr_t)entry->pixels,
                       tile->width, tile->height);
        sr_blit(canvas, &source_canvas, tile->x, tile->y);
    } else {
        sr_fill_rect(canvas, (float)tile->x, (float)tile->y,
                     (float)tile->width, (float)tile->height,
                     TILE_EMPTY, 1.0f);
    }
    draw_caption(canvas, entry);
}

bool krtsp_compose(krtsp_compositor *compositor,
                   const krtsp_compose_tile *tiles, size_t count,
                   uint8_t *rgba_out, size_t rgba_capacity)
{
    sr_canvas canvas;

    if (compositor == NULL || compositor->pixels == NULL || tiles == NULL ||
        count == 0u || count > KRTSP_COMPOSE_MAX || rgba_out == NULL) {
        return false;
    }
    for (size_t index = 0u; index < count; ++index) {
        if (tiles[index].caption == NULL) {
            return false;
        }
    }

    sr_canvas_wrap(&canvas, compositor->pixels, compositor->width,
                   compositor->height);
    sr_clear(&canvas, CANVAS_CLEAR);
    for (size_t index = 0u; index < count; ++index) {
        draw_tile(&canvas, &tiles[index]);
    }
    return sr_pack_rgba(&canvas, rgba_out, rgba_capacity);
}
