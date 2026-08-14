/*
 * Throughput of the mosaic composite: the seven-camera grid the README
 * calls the real case, at a full-HD canvas, driven the way the view
 * loop drives it - everything changing, one tile changing, and only a
 * caption's age counter changing.
 *
 * Numbers, not thresholds; see bench_frame.c for why.  The checksum is
 * printed so two runs can also be compared for identical output.
 */

#include "krtsp_compose.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CANVAS_W 1920
#define CANVAS_H 1080
#define TILE_COUNT 7u
#define WIDE (16.0f / 9.0f)

static double monotonic_seconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1.0;
    }
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static uint64_t buffer_checksum(const uint8_t *bytes, size_t count)
{
    uint64_t checksum = 0u;

    for (size_t index = 0u; index < count; index += 4093u) {
        checksum = checksum * UINT64_C(0x100000001b3) ^ bytes[index];
    }
    return checksum;
}

typedef struct rig {
    krtsp_tile tiles[KRTSP_COMPOSE_MAX];
    uint8_t *frames[KRTSP_COMPOSE_MAX];
    uint64_t sequences[KRTSP_COMPOSE_MAX];
    char captions[KRTSP_COMPOSE_MAX][64];
    uint32_t accents[KRTSP_COMPOSE_MAX];
    krtsp_compositor compositor;
    uint8_t *out;
    size_t out_size;
} rig;

static bool compose_rig(rig *bench)
{
    krtsp_compose_tile inputs[KRTSP_COMPOSE_MAX];
    krtsp_damage damage[KRTSP_COMPOSE_MAX + 1];
    size_t damage_count = 0u;

    for (size_t index = 0u; index < TILE_COUNT; ++index) {
        inputs[index].tile = bench->tiles[index];
        inputs[index].pixels = bench->frames[index];
        inputs[index].sequence = bench->sequences[index];
        inputs[index].caption = bench->captions[index];
        inputs[index].accent = bench->accents[index];
    }
    return krtsp_compose(&bench->compositor, inputs, TILE_COUNT,
                         bench->out, bench->out_size, damage,
                         KRTSP_COMPOSE_MAX + 1u, &damage_count);
}

static bool report(const char *name, size_t iterations, double started,
                   const rig *bench)
{
    const double elapsed = monotonic_seconds() - started;

    if (!(elapsed > 0.0)) {
        return false;
    }
    (void)printf(
        "%s iterations=%zu composites_s=%.1f mean_ms=%.3f "
        "checksum=%016" PRIx64 "\n",
        name, iterations, (double)iterations / elapsed,
        elapsed * 1000.0 / (double)iterations,
        buffer_checksum(bench->out, bench->out_size));
    return true;
}

static bool bench_all_tiles_new(rig *bench)
{
    const size_t iterations = 100u;
    const double started = monotonic_seconds();

    for (size_t iteration = 0u; iteration < iterations; ++iteration) {
        for (size_t index = 0u; index < TILE_COUNT; ++index) {
            ++bench->sequences[index];
            bench->frames[index][0] = (uint8_t)iteration;
        }
        if (!compose_rig(bench)) {
            return false;
        }
    }
    return report("all_tiles_new", iterations, started, bench);
}

static bool bench_one_tile_new(rig *bench)
{
    const size_t iterations = 400u;
    const double started = monotonic_seconds();

    for (size_t iteration = 0u; iteration < iterations; ++iteration) {
        const size_t index = iteration % TILE_COUNT;

        ++bench->sequences[index];
        bench->frames[index][0] = (uint8_t)iteration;
        if (!compose_rig(bench)) {
            return false;
        }
    }
    return report("one_tile_new", iterations, started, bench);
}

static bool bench_caption_only(rig *bench)
{
    const size_t iterations = 400u;
    const double started = monotonic_seconds();

    for (size_t iteration = 0u; iteration < iterations; ++iteration) {
        (void)snprintf(bench->captions[3], sizeof(bench->captions[3]),
                       "camera 3  stale  %zus", iteration);
        if (!compose_rig(bench)) {
            return false;
        }
    }
    return report("caption_only", iterations, started, bench);
}

int
main(void)
{
    rig bench;
    bool ok = true;

    (void)memset(&bench, 0, sizeof(bench));
    if (krtsp_mosaic_layout(CANVAS_W, CANVAS_H, TILE_COUNT, WIDE,
                            bench.tiles, KRTSP_COMPOSE_MAX) != TILE_COUNT) {
        (void)fprintf(stderr, "cannot lay out the mosaic\n");
        return 1;
    }
    bench.out_size = (size_t)CANVAS_W * (size_t)CANVAS_H * 4u;
    bench.out = malloc(bench.out_size);
    if (bench.out == NULL ||
        !krtsp_compositor_init(&bench.compositor, CANVAS_W, CANVAS_H)) {
        (void)fprintf(stderr, "cannot build the compositor\n");
        free(bench.out);
        return 1;
    }
    for (size_t index = 0u; index < TILE_COUNT; ++index) {
        const size_t bytes = (size_t)bench.tiles[index].width *
                             (size_t)bench.tiles[index].height * 4u;

        bench.frames[index] = malloc(bytes);
        if (bench.frames[index] == NULL) {
            (void)fprintf(stderr, "cannot build the tile frames\n");
            return 1;
        }
        for (size_t at = 0u; at < bytes; ++at) {
            bench.frames[index][at] = (uint8_t)(at * 31u + index);
        }
        bench.sequences[index] = 1u;
        (void)snprintf(bench.captions[index], sizeof(bench.captions[index]),
                       "camera %zu", index);
        bench.accents[index] = 0x40C060u;
    }

    (void)printf("mosaic composite, %dx%d, %zu tiles\n",
                 CANVAS_W, CANVAS_H, (size_t)TILE_COUNT);
    ok = ok && bench_all_tiles_new(&bench);
    ok = ok && bench_one_tile_new(&bench);
    ok = ok && bench_caption_only(&bench);

    for (size_t index = 0u; index < TILE_COUNT; ++index) {
        free(bench.frames[index]);
    }
    krtsp_compositor_free(&bench.compositor);
    free(bench.out);
    return ok ? 0 : 1;
}
