/*
 * The mosaic composite against a reference implementation.
 *
 * The reference below is a transcription of the original in-loop
 * composite: wrap the output buffer as the canvas, clear the whole
 * thing, blit every tile, caption every tile, pack in place.  It is
 * deliberately naive and rebuilt from nothing on every call, which makes
 * it history-free: whatever shortcuts the compositor takes across calls,
 * its output must equal what the full rebuild would have produced.
 * Every test drives both over the same inputs and compares whole output
 * buffers, byte for byte.
 */

#include "krtsp_compose.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

#define WIDE (16.0f / 9.0f)

/* ---------------------------- reference --------------------------------- */

static void reference_caption(sr_canvas *canvas,
                              const krtsp_compose_tile *entry)
{
    const krtsp_tile *tile = &entry->tile;
    int pad = 4;
    int band = SR_FONT_H + pad;

    for (int y = 0; y < band && tile->y + y < canvas->h; ++y) {
        for (int x = 0; x < tile->width; ++x) {
            sr_blend(canvas, tile->x + x, tile->y + y, 0x000000u, 0.5f);
        }
    }
    sr_fill_rect(canvas, (float)tile->x, (float)tile->y, 3.0f, (float)band,
                 entry->accent, 1.0f);
    sr_text_shadow(canvas, (float)(tile->x + pad + 3),
                   (float)(tile->y + pad / 2), entry->caption,
                   0xFFFFFFu, 1.0f, 1);
}

static bool reference_compose(const krtsp_compose_tile *tiles, size_t count,
                              int width, int height, uint8_t *out,
                              size_t out_size)
{
    sr_canvas canvas;

    sr_canvas_wrap(&canvas, (uint32_t *)(void *)out, width, height);
    sr_clear(&canvas, 0x0A0A0Cu);
    for (size_t index = 0u; index < count; ++index) {
        const krtsp_tile *tile = &tiles[index].tile;

        if (tiles[index].pixels != NULL) {
            sr_canvas source_canvas;

            sr_canvas_wrap(&source_canvas,
                           (uint32_t *)(void *)(uintptr_t)
                               tiles[index].pixels,
                           tile->width, tile->height);
            sr_blit(&canvas, &source_canvas, tile->x, tile->y);
        } else {
            sr_fill_rect(&canvas, (float)tile->x, (float)tile->y,
                         (float)tile->width, (float)tile->height,
                         0x141418u, 1.0f);
        }
        reference_caption(&canvas, &tiles[index]);
    }
    return sr_pack_rgba(&canvas, out, out_size);
}

/* ------------------------------ scenery --------------------------------- */

static uint32_t rng_state = 0x2545F491u;

static uint32_t rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void fill_frame(uint8_t *bgra, int width, int height, uint32_t seed)
{
    size_t bytes = (size_t)width * (size_t)height * 4u;

    rng_state = seed != 0u ? seed : 1u;
    for (size_t index = 0u; index < bytes; ++index) {
        bgra[index] = (uint8_t)rng_next();
    }
}

/* One mosaic being fed to both implementations.  The compositor lives
 * across steps; the reference is rebuilt fresh each step. */
typedef struct scene {
    int width;
    int height;
    size_t count;
    krtsp_tile tiles[KRTSP_COMPOSE_MAX];
    uint8_t *frames[KRTSP_COMPOSE_MAX];
    bool present[KRTSP_COMPOSE_MAX];
    uint64_t sequences[KRTSP_COMPOSE_MAX];
    char captions[KRTSP_COMPOSE_MAX][192];
    uint32_t accents[KRTSP_COMPOSE_MAX];
    krtsp_compositor compositor;
    uint8_t *out_new;
    uint8_t *out_ref;
    size_t out_size;
} scene;

static void scene_drop(scene *sc)
{
    for (size_t index = 0u; index < sc->count; ++index) {
        free(sc->frames[index]);
        sc->frames[index] = NULL;
    }
    krtsp_compositor_free(&sc->compositor);
    free(sc->out_new);
    free(sc->out_ref);
    sc->out_new = NULL;
    sc->out_ref = NULL;
}

static bool scene_build(scene *sc, int width, int height, size_t count)
{
    (void)memset(sc, 0, sizeof(*sc));
    sc->width = width;
    sc->height = height;
    sc->count = count;
    sc->out_size = (size_t)width * (size_t)height * 4u;
    CHECK(krtsp_mosaic_layout(width, height, count, WIDE, sc->tiles,
                              KRTSP_COMPOSE_MAX) == count);
    CHECK(krtsp_compositor_init(&sc->compositor, width, height));
    sc->out_new = malloc(sc->out_size);
    sc->out_ref = malloc(sc->out_size);
    CHECK(sc->out_new != NULL && sc->out_ref != NULL);
    for (size_t index = 0u; index < count; ++index) {
        size_t bytes = (size_t)sc->tiles[index].width *
                       (size_t)sc->tiles[index].height * 4u;

        sc->frames[index] = malloc(bytes);
        CHECK(sc->frames[index] != NULL);
        fill_frame(sc->frames[index], sc->tiles[index].width,
                   sc->tiles[index].height, (uint32_t)(index + 1u) * 977u);
        sc->present[index] = true;
        sc->sequences[index] = 1u;
        (void)snprintf(sc->captions[index], sizeof(sc->captions[index]),
                       "camera %zu", index);
        sc->accents[index] = 0x40C060u;
    }
    return true;
}

static void scene_inputs(const scene *sc,
                         krtsp_compose_tile inputs[KRTSP_COMPOSE_MAX])
{
    for (size_t index = 0u; index < sc->count; ++index) {
        inputs[index].tile = sc->tiles[index];
        inputs[index].pixels =
            sc->present[index] ? sc->frames[index] : NULL;
        inputs[index].sequence = sc->sequences[index];
        inputs[index].caption = sc->captions[index];
        inputs[index].accent = sc->accents[index];
    }
}

/* Compose both ways and demand identical bytes. */
static bool scene_agrees(scene *sc)
{
    krtsp_compose_tile inputs[KRTSP_COMPOSE_MAX];

    scene_inputs(sc, inputs);
    CHECK(krtsp_compose(&sc->compositor, inputs, sc->count, sc->out_new,
                        sc->out_size));
    CHECK(reference_compose(inputs, sc->count, sc->width, sc->height,
                            sc->out_ref, sc->out_size));
    CHECK(memcmp(sc->out_new, sc->out_ref, sc->out_size) == 0);
    return true;
}

/* ------------------------------- tests ---------------------------------- */

static bool test_matches_the_reference(void)
{
    scene sc;
    bool ok;

    CHECK(scene_build(&sc, 1280, 720, 7u));
    /* Mixed states on the very first composite: one camera absent, one
     * degraded with an age in its caption. */
    sc.present[3] = false;
    (void)snprintf(sc.captions[3], sizeof(sc.captions[3]),
                   "camera 3  connecting");
    sc.accents[3] = 0xC0A040u;
    (void)snprintf(sc.captions[5], sizeof(sc.captions[5]),
                   "camera 5  stale  12s");
    sc.accents[5] = 0xE08030u;
    ok = scene_agrees(&sc);
    scene_drop(&sc);
    return ok;
}

static bool test_single_tile_fills_the_canvas(void)
{
    scene sc;
    bool ok;

    CHECK(scene_build(&sc, 640, 360, 1u));
    ok = scene_agrees(&sc);
    scene_drop(&sc);
    return ok;
}

static bool test_update_sequence_stays_identical(void)
{
    scene sc;

    CHECK(scene_build(&sc, 1280, 720, 7u));
    CHECK(scene_agrees(&sc));

    /* One tile gets a new frame; the other six do not. */
    sc.sequences[2] += 1u;
    fill_frame(sc.frames[2], sc.tiles[2].width, sc.tiles[2].height, 5077u);
    CHECK(scene_agrees(&sc));

    /* Only a caption ages: the frozen-camera second counter. */
    (void)snprintf(sc.captions[4], sizeof(sc.captions[4]),
                   "camera 4  stale  3s");
    sc.accents[4] = 0xE08030u;
    CHECK(scene_agrees(&sc));
    (void)snprintf(sc.captions[4], sizeof(sc.captions[4]),
                   "camera 4  stale  4s");
    CHECK(scene_agrees(&sc));

    /* A camera drops out entirely, then comes back. */
    sc.present[5] = false;
    (void)snprintf(sc.captions[5], sizeof(sc.captions[5]),
                   "camera 5  offline");
    sc.accents[5] = 0xD04040u;
    CHECK(scene_agrees(&sc));
    sc.present[5] = true;
    sc.sequences[5] += 1u;
    fill_frame(sc.frames[5], sc.tiles[5].width, sc.tiles[5].height, 9199u);
    (void)snprintf(sc.captions[5], sizeof(sc.captions[5]), "camera 5");
    sc.accents[5] = 0x40C060u;
    CHECK(scene_agrees(&sc));

    /* Only an accent changes. */
    sc.accents[0] = 0xC0A040u;
    CHECK(scene_agrees(&sc));

    /* Nothing changes at all: composing again must be idempotent. */
    CHECK(scene_agrees(&sc));

    /* Everything changes at once. */
    for (size_t index = 0u; index < sc.count; ++index) {
        sc.sequences[index] += 1u;
        fill_frame(sc.frames[index], sc.tiles[index].width,
                   sc.tiles[index].height, (uint32_t)(index + 41u) * 131u);
    }
    CHECK(scene_agrees(&sc));

    scene_drop(&sc);
    return true;
}

static bool test_wide_caption_still_matches(void)
{
    scene sc;

    /* Sixteen tiles on a small canvas leaves each one far narrower than
     * this caption, so the text runs past its tile - the reference
     * clips it against the canvas alone and repaints everything, and
     * whatever the compositor does instead must produce the same bytes. */
    CHECK(scene_build(&sc, 640, 360, 16u));
    (void)snprintf(sc.captions[1], sizeof(sc.captions[1]),
                   "a very long camera label that cannot possibly fit  "
                   "offline  120s");
    sc.accents[1] = 0xD04040u;
    CHECK(scene_agrees(&sc));

    /* An unrelated tile updates while the runaway caption stays. */
    sc.sequences[9] += 1u;
    fill_frame(sc.frames[9], sc.tiles[9].width, sc.tiles[9].height, 613u);
    CHECK(scene_agrees(&sc));

    /* The runaway caption itself changes - and then shrinks, which must
     * erase the overhang it previously painted. */
    (void)snprintf(sc.captions[1], sizeof(sc.captions[1]),
                   "a very long camera label that cannot possibly fit  "
                   "offline  121s");
    CHECK(scene_agrees(&sc));
    (void)snprintf(sc.captions[1], sizeof(sc.captions[1]), "short");
    sc.accents[1] = 0x40C060u;
    CHECK(scene_agrees(&sc));

    scene_drop(&sc);
    return true;
}

/* The translucent plates under captions and banners: an alpha fill and
 * a per-pixel blend loop must lay down identical pixels, both inside
 * the canvas and clipped at its edges. */
static bool test_plate_fill_equals_blend_loop(void)
{
    enum { W = 320, H = 200 };
    static uint32_t looped[W * H];
    static uint32_t filled[W * H];
    sr_canvas canvas_a;
    sr_canvas canvas_b;

    rng_state = 71u;
    for (size_t index = 0u; index < (size_t)W * H; ++index) {
        looped[index] = rng_next();
    }
    (void)memcpy(filled, looped, sizeof(filled));
    sr_canvas_wrap(&canvas_a, looped, W, H);
    sr_canvas_wrap(&canvas_b, filled, W, H);

    /* The caption band: an interior rect at alpha 0.5. */
    for (int y = 0; y < 20 && 40 + y < H; ++y) {
        for (int x = 0; x < 150; ++x) {
            sr_blend(&canvas_a, 60 + x, 40 + y, 0x000000u, 0.5f);
        }
    }
    sr_fill_rect(&canvas_b, 60.0f, 40.0f, 150.0f, 20.0f, 0x000000u, 0.5f);
    CHECK(memcmp(looped, filled, sizeof(looped)) == 0);

    /* The view banner: full width at alpha 0.55, taller than the
     * canvas, so the loop's clamp and the fill's clip must agree. */
    for (int y = 0; y < 260 && y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            sr_blend(&canvas_a, x, y, 0x000000u, 0.55f);
        }
    }
    sr_fill_rect(&canvas_b, 0.0f, 0.0f, (float)W, 260.0f, 0x000000u, 0.55f);
    CHECK(memcmp(looped, filled, sizeof(looped)) == 0);

    /* A band crossing the bottom edge, as a tile touching the canvas
     * bottom would produce. */
    for (int y = 0; y < 20 && H - 10 + y < H; ++y) {
        for (int x = 0; x < 80; ++x) {
            sr_blend(&canvas_a, 10 + x, H - 10 + y, 0x000000u, 0.5f);
        }
    }
    sr_fill_rect(&canvas_b, 10.0f, (float)(H - 10), 80.0f, 20.0f,
                 0x000000u, 0.5f);
    CHECK(memcmp(looped, filled, sizeof(looped)) == 0);

    return true;
}

static bool test_refusals(void)
{
    scene sc;
    krtsp_compose_tile inputs[KRTSP_COMPOSE_MAX];
    krtsp_compositor dead;
    uint8_t sliver[16];

    CHECK(scene_build(&sc, 640, 360, 2u));
    scene_inputs(&sc, inputs);

    CHECK(!krtsp_compose(&sc.compositor, inputs, 0u, sc.out_new,
                         sc.out_size));
    CHECK(!krtsp_compose(&sc.compositor, inputs, KRTSP_COMPOSE_MAX + 1u,
                         sc.out_new, sc.out_size));
    CHECK(!krtsp_compose(&sc.compositor, NULL, sc.count, sc.out_new,
                         sc.out_size));
    CHECK(!krtsp_compose(&sc.compositor, inputs, sc.count, NULL,
                         sc.out_size));
    CHECK(!krtsp_compose(&sc.compositor, inputs, sc.count, sliver,
                         sizeof(sliver)));
    inputs[1].caption = NULL;
    CHECK(!krtsp_compose(&sc.compositor, inputs, sc.count, sc.out_new,
                         sc.out_size));
    inputs[1].caption = sc.captions[1];

    (void)memset(&dead, 0, sizeof(dead));
    CHECK(!krtsp_compose(&dead, inputs, sc.count, sc.out_new, sc.out_size));
    CHECK(!krtsp_compositor_init(NULL, 640, 360));
    CHECK(!krtsp_compositor_init(&dead, 0, 360));
    CHECK(!krtsp_compositor_init(&dead, 640, -1));

    /* And after all the refusals, a real composite still works. */
    CHECK(scene_agrees(&sc));
    scene_drop(&sc);
    return true;
}

typedef bool (*test_function)(void);

typedef struct test_case {
    const char *name;
    test_function function;
} test_case;

int
main(void)
{
    static const test_case tests[] = {
        {"matches the reference", test_matches_the_reference},
        {"single tile fills the canvas", test_single_tile_fills_the_canvas},
        {"update sequence stays identical",
         test_update_sequence_stays_identical},
        {"wide caption still matches", test_wide_caption_still_matches},
        {"plate fill equals blend loop", test_plate_fill_equals_blend_loop},
        {"refusals", test_refusals}
    };
    size_t passed = 0u;

    for (size_t index = 0u; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        const bool ok = tests[index].function();

        (void)printf("%s %s\n", ok ? "ok" : "not ok", tests[index].name);
        if (!ok) {
            return 1;
        }
        ++passed;
    }
    (void)printf("%zu tests passed\n", passed);
    return 0;
}
