/*
 * See krtsp_compose.h.  The drawing here defines the mosaic's pixels:
 * a dark canvas, one blit per tile (or a placeholder fill when a camera
 * has produced nothing), and a captioned band across each tile's top -
 * a translucent plate, a status-colored bar, and the label text.
 *
 * The canvas persists, so each composite redraws the least it can prove
 * enough: a tile whose frame changed is re-blitted and re-captioned; a
 * tile whose caption or accent changed gets only its band restored and
 * redrawn; an untouched tile is not touched.  test_compose holds every
 * shortcut to the bytes a naive full rebuild produces.
 */

#include "krtsp_compose.h"

#include <stdlib.h>
#include <string.h>

#define CANVAS_CLEAR 0x0A0A0Cu
#define TILE_EMPTY   0x141418u
#define CAPTION_PAD  4
#define CAPTION_BAND (SR_FONT_H + CAPTION_PAD)

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
    (void)memset(compositor, 0, sizeof(*compositor));
    compositor->pixels = malloc(bytes);
    if (compositor->pixels == NULL) {
        return false;
    }
    compositor->width = width;
    compositor->height = height;
    compositor->fresh = true;
    return true;
}

void krtsp_compositor_reset(krtsp_compositor *compositor)
{
    if (compositor == NULL) {
        return;
    }
    compositor->fresh = true;
    compositor->count = 0u;
}

void krtsp_compositor_free(krtsp_compositor *compositor)
{
    if (compositor == NULL) {
        return;
    }
    free(compositor->pixels);
    (void)memset(compositor, 0, sizeof(*compositor));
}

/* Every tile is captioned.  In a grid, "which camera is that" is the
 * first question, and an unlabelled tile that has frozen is
 * indistinguishable from a quiet scene. */
static void draw_caption(sr_canvas *canvas, const krtsp_compose_tile *entry)
{
    const krtsp_tile *tile = &entry->tile;

    /* A translucent plate keeps the text readable over a bright scene
     * without hiding the top of the picture.  For an integer-aligned
     * rect the fill's edge coverage is exactly 1, so this lays down the
     * same pixels a per-pixel blend loop would - test_compose holds it
     * to that - without a call and a clip check per pixel. */
    sr_fill_rect(canvas, (float)tile->x, (float)tile->y,
                 (float)tile->width, (float)CAPTION_BAND, 0x000000u, 0.5f);
    sr_fill_rect(canvas, (float)tile->x, (float)tile->y, 3.0f,
                 (float)CAPTION_BAND, entry->accent, 1.0f);
    sr_text_shadow(canvas, (float)(tile->x + CAPTION_PAD + 3),
                   (float)(tile->y + CAPTION_PAD / 2), entry->caption,
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

/* Restore what sits under the caption band - the top strip of the tile's
 * frame, or the placeholder color - then redraw the caption over it.
 * The plate is translucent, so drawing it twice would darken twice. */
static void redraw_band(sr_canvas *canvas, const krtsp_compose_tile *entry)
{
    const krtsp_tile *tile = &entry->tile;

    if (entry->pixels != NULL) {
        sr_canvas strip;

        sr_canvas_wrap(&strip, (uint32_t *)(void *)(uintptr_t)entry->pixels,
                       tile->width, CAPTION_BAND);
        sr_blit(canvas, &strip, tile->x, tile->y);
    } else {
        sr_fill_rect(canvas, (float)tile->x, (float)tile->y,
                     (float)tile->width, (float)CAPTION_BAND,
                     TILE_EMPTY, 1.0f);
    }
    draw_caption(canvas, entry);
}

/* Text is clipped against the canvas, not the tile, so a caption longer
 * than its tile paints an overhang the incremental paths cannot account
 * for.  Fit means the band and the text (shadow included) stay inside
 * the tile. */
static bool caption_fits(const krtsp_compose_tile *entry)
{
    const krtsp_tile *tile = &entry->tile;
    int text_width = sr_text_width(entry->caption, 1);

    if (CAPTION_BAND > tile->height) {
        return false;
    }
    return text_width <= tile->width - (CAPTION_PAD + 3 + 1);
}

static bool same_geometry(const krtsp_tile *a, const krtsp_tile *b)
{
    return a->x == b->x && a->y == b->y &&
           a->width == b->width && a->height == b->height;
}

static bool caption_matches(const krtsp_compose_slot *slot,
                            const char *caption)
{
    if (slot->caption_overlong ||
        strlen(caption) >= KRTSP_COMPOSE_CAPTION_MAX) {
        return false;
    }
    return strcmp(slot->caption, caption) == 0;
}

static void remember(krtsp_compose_slot *slot,
                     const krtsp_compose_tile *entry)
{
    size_t length = strlen(entry->caption);

    slot->tile = entry->tile;
    slot->sequence = entry->sequence;
    slot->accent = entry->accent;
    slot->has_pixels = entry->pixels != NULL;
    slot->caption_overlong = length >= KRTSP_COMPOSE_CAPTION_MAX;
    if (slot->caption_overlong) {
        slot->caption[0] = '\0';
    } else {
        (void)memcpy(slot->caption, entry->caption, length + 1u);
    }
}

typedef struct damage_sink {
    krtsp_damage *rects;
    size_t capacity;
    size_t count;
    bool overflowed;
} damage_sink;

static void damage_push(damage_sink *sink, int x0, int y0, int x1, int y1)
{
    if (sink->rects == NULL) {
        return;
    }
    if (sink->count >= sink->capacity) {
        sink->overflowed = true;
        return;
    }
    sink->rects[sink->count].x0 = x0;
    sink->rects[sink->count].y0 = y0;
    sink->rects[sink->count].x1 = x1;
    sink->rects[sink->count].y1 = y1;
    ++sink->count;
}

bool krtsp_compose(krtsp_compositor *compositor,
                   const krtsp_compose_tile *tiles, size_t count,
                   uint8_t *rgba_out, size_t rgba_capacity,
                   krtsp_damage *damage, size_t damage_capacity,
                   size_t *damage_count)
{
    sr_canvas canvas;
    damage_sink sink;
    size_t needed;
    bool fits = true;
    bool full;

    if (compositor == NULL || compositor->pixels == NULL || tiles == NULL ||
        count == 0u || count > KRTSP_COMPOSE_MAX || rgba_out == NULL ||
        !canvas_size(compositor->width, compositor->height, &needed) ||
        rgba_capacity < needed ||
        (damage != NULL && (damage_count == NULL || damage_capacity == 0u))) {
        return false;
    }
    for (size_t index = 0u; index < count; ++index) {
        if (tiles[index].caption == NULL) {
            return false;
        }
    }

    sink.rects = damage;
    sink.capacity = damage != NULL ? damage_capacity : 0u;
    sink.count = 0u;
    sink.overflowed = false;

    for (size_t index = 0u; index < count; ++index) {
        if (!caption_fits(&tiles[index])) {
            fits = false;
        }
    }
    full = compositor->fresh || !fits || count != compositor->count;
    for (size_t index = 0u; index < count && !full; ++index) {
        if (!same_geometry(&compositor->slots[index].tile,
                           &tiles[index].tile)) {
            full = true;
        }
    }

    sr_canvas_wrap(&canvas, compositor->pixels, compositor->width,
                   compositor->height);
    if (full) {
        sr_clear(&canvas, CANVAS_CLEAR);
        for (size_t index = 0u; index < count; ++index) {
            draw_tile(&canvas, &tiles[index]);
        }
        damage_push(&sink, 0, 0, compositor->width, compositor->height);
    } else {
        for (size_t index = 0u; index < count; ++index) {
            const krtsp_compose_tile *entry = &tiles[index];
            const krtsp_compose_slot *slot = &compositor->slots[index];
            const krtsp_tile *tile = &entry->tile;
            bool has_pixels = entry->pixels != NULL;
            bool frame_changed =
                has_pixels != slot->has_pixels ||
                (has_pixels && entry->sequence != slot->sequence);

            if (frame_changed) {
                draw_tile(&canvas, entry);
                damage_push(&sink, tile->x, tile->y,
                            tile->x + tile->width, tile->y + tile->height);
            } else if (entry->accent != slot->accent ||
                       !caption_matches(slot, entry->caption)) {
                redraw_band(&canvas, entry);
                damage_push(&sink, tile->x, tile->y,
                            tile->x + tile->width, tile->y + CAPTION_BAND);
            }
        }
    }
    for (size_t index = 0u; index < count; ++index) {
        remember(&compositor->slots[index], &tiles[index]);
    }
    compositor->count = count;
    /* A spilled caption leaves an overhang the slots do not describe,
     * so keep rebuilding until every caption fits again. */
    compositor->fresh = !fits;

    if (sink.overflowed) {
        sink.rects[0].x0 = 0;
        sink.rects[0].y0 = 0;
        sink.rects[0].x1 = compositor->width;
        sink.rects[0].y1 = compositor->height;
        sink.count = 1u;
    }
    if (damage != NULL) {
        *damage_count = sink.count;
    }
    return sr_pack_rgba(&canvas, rgba_out, rgba_capacity);
}
