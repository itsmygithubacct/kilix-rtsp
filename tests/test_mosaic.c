#include "kilix_rtsp.h"

#include <stdio.h>
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

static krtsp_tile tiles[64];

static bool overlaps(const krtsp_tile *a, const krtsp_tile *b)
{
    return a->x < b->x + b->width && b->x < a->x + a->width &&
           a->y < b->y + b->height && b->y < a->y + a->height;
}

/* The invariants that make a mosaic a mosaic, checked for every layout. */
static bool layout_is_sane(size_t placed, int canvas_w, int canvas_h)
{
    for (size_t i = 0u; i < placed; ++i) {
        CHECK(tiles[i].width > 0 && tiles[i].height > 0);
        /* Even dimensions: odd sizes upset some ffmpeg scalers, and each
         * tile is a decode target. */
        CHECK(tiles[i].width % 2 == 0);
        CHECK(tiles[i].height % 2 == 0);
        /* Inside the canvas. */
        CHECK(tiles[i].x >= 0 && tiles[i].y >= 0);
        CHECK(tiles[i].x + tiles[i].width <= canvas_w);
        CHECK(tiles[i].y + tiles[i].height <= canvas_h);
        /* Distinct source per tile. */
        CHECK(tiles[i].index == i);
        for (size_t j = i + 1u; j < placed; ++j) {
            CHECK(!overlaps(&tiles[i], &tiles[j]));
        }
    }
    return true;
}

static bool
test_counts_one_to_sixteen(void)
{
    /* Every count has to produce a usable grid; the awkward ones are the
     * primes and the counts just past a square. */
    for (size_t count = 1u; count <= 16u; ++count) {
        size_t placed = krtsp_mosaic_layout(1920, 1080, count, WIDE, tiles,
                                            sizeof(tiles) / sizeof(tiles[0]));

        CHECK(placed == count);
        CHECK(layout_is_sane(placed, 1920, 1080));
    }
    return true;
}

static bool
test_single_source_fills_the_canvas(void)
{
    size_t placed = krtsp_mosaic_layout(1920, 1080, 1u, WIDE, tiles, 64u);

    CHECK(placed == 1u);
    /* One camera should not be shrunk into a corner. */
    CHECK(tiles[0].width == 1920);
    CHECK(tiles[0].height == 1080);
    CHECK(tiles[0].x == 0 && tiles[0].y == 0);
    return true;
}

static bool
test_column_choice_follows_the_canvas(void)
{
    /* Four cameras on a square canvas want 2x2. */
    CHECK(krtsp_mosaic_columns(1000, 1000, 4u, WIDE) == 2u);

    /* The same four on a very wide canvas do NOT want 2x2: that would
     * make each tile twice as wide as the picture in it.  A single row
     * keeps the tiles close to the camera's own shape. */
    CHECK(krtsp_mosaic_columns(3840, 600, 4u, WIDE) == 4u);

    /* And on a tall, narrow canvas they stack. */
    CHECK(krtsp_mosaic_columns(600, 3000, 4u, WIDE) == 1u);
    return true;
}

static bool
test_seven_cameras_the_real_case(void)
{
    /* The reference fleet is seven cameras. */
    size_t placed = krtsp_mosaic_layout(1908, 1008, 7u, WIDE, tiles, 64u);
    size_t columns = krtsp_mosaic_columns(1908, 1008, 7u, WIDE);
    size_t last_row_count;

    CHECK(placed == 7u);
    CHECK(layout_is_sane(placed, 1908, 1008));

    /* A short final row must be centred, not left-aligned. */
    last_row_count = 7u % columns;
    if (last_row_count != 0u) {
        int first_full_row_x = tiles[0].x;
        int last_row_x = tiles[7u - last_row_count].x;

        CHECK(last_row_x > first_full_row_x);
    }
    return true;
}

static bool
test_tiles_are_much_cheaper_than_fullscreen(void)
{
    /* Not a layout rule but the reason the mosaic is affordable at all:
     * seven tiles hold far fewer pixels than one full-screen camera, and
     * decode cost follows output pixels.  Measured: one camera scaled to
     * 1908x1008 costs 21-44% of a core, while all seven at tile size cost
     * 0.58 of a core combined. */
    size_t placed = krtsp_mosaic_layout(1908, 1008, 7u, WIDE, tiles, 64u);
    long long tile_pixels = 0;
    long long full_pixels = 1908LL * 1008LL;

    CHECK(placed == 7u);
    for (size_t i = 0u; i < placed; ++i) {
        tile_pixels += (long long)tiles[i].width * tiles[i].height;
    }
    /* Seven tiles cover at most the canvas, never more. */
    CHECK(tile_pixels <= full_pixels);
    /* And each individual tile is a small fraction of it. */
    CHECK((long long)tiles[0].width * tiles[0].height < full_pixels / 4);
    return true;
}

static bool
test_small_canvas_and_rejections(void)
{
    /* A canvas too small to hold the requested grid must fail rather than
     * emit zero-sized tiles that would become invalid ffmpeg scale args. */
    CHECK(krtsp_mosaic_layout(4, 4, 16u, WIDE, tiles, 64u) == 0u);

    /* Capacity is respected. */
    CHECK(krtsp_mosaic_layout(1920, 1080, 8u, WIDE, tiles, 4u) == 0u);

    CHECK(krtsp_mosaic_layout(0, 1080, 4u, WIDE, tiles, 64u) == 0u);
    CHECK(krtsp_mosaic_layout(1920, 0, 4u, WIDE, tiles, 64u) == 0u);
    CHECK(krtsp_mosaic_layout(1920, 1080, 0u, WIDE, tiles, 64u) == 0u);
    CHECK(krtsp_mosaic_layout(1920, 1080, 4u, WIDE, NULL, 64u) == 0u);

    /* A nonsense aspect falls back to 16:9 rather than dividing by zero. */
    CHECK(krtsp_mosaic_layout(1920, 1080, 4u, 0.0f, tiles, 64u) == 4u);
    CHECK(krtsp_mosaic_layout(1920, 1080, 4u, -3.0f, tiles, 64u) == 4u);

    CHECK(krtsp_mosaic_columns(0, 0, 4u, WIDE) == 0u);
    return true;
}

static bool
test_odd_canvas_stays_inside(void)
{
    /* Odd canvas dimensions are the usual source of off-by-one overflow,
     * because tiles are forced even. */
    const int widths[] = {1919, 1001, 777, 1908};
    const int heights[] = {1079, 601, 333, 1008};

    for (size_t w = 0u; w < sizeof(widths) / sizeof(widths[0]); ++w) {
        for (size_t h = 0u; h < sizeof(heights) / sizeof(heights[0]); ++h) {
            for (size_t count = 1u; count <= 9u; ++count) {
                size_t placed = krtsp_mosaic_layout(
                    widths[w], heights[h], count, WIDE, tiles, 64u);

                if (placed == 0u) {
                    continue;   /* too small for that many, already tested */
                }
                CHECK(placed == count);
                CHECK(layout_is_sane(placed, widths[w], heights[h]));
            }
        }
    }
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
        {"counts one to sixteen", test_counts_one_to_sixteen},
        {"single source fills the canvas",
         test_single_source_fills_the_canvas},
        {"column choice follows the canvas",
         test_column_choice_follows_the_canvas},
        {"seven cameras, the real case", test_seven_cameras_the_real_case},
        {"tiles are much cheaper than fullscreen",
         test_tiles_are_much_cheaper_than_fullscreen},
        {"small canvas and rejections", test_small_canvas_and_rejections},
        {"odd canvas stays inside", test_odd_canvas_stays_inside}
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
