/*
 * Grid layout for a camera mosaic.
 *
 * Pure arithmetic, no canvas and no sources, because layout is where a
 * mosaic is actually right or wrong: overlapping tiles, tiles off the
 * canvas, and distorted pictures are all layout bugs, and none of them
 * need a terminal to reproduce.
 *
 * Choosing the column count is the only interesting decision.  A plain
 * ceil(sqrt(n)) ignores the canvas shape and gives badly proportioned
 * tiles on a wide terminal - four cameras in a 2x2 on a 21:9 screen
 * leaves each tile twice as wide as the picture in it.  Instead every
 * column count is tried and the one whose resulting tile is closest to
 * the source aspect wins, which is the same idea as Frigate's Birdseye
 * scaling coefficient arrived at from the other direction.
 */

#include "kilix_rtsp.h"

#include <math.h>

/* Even sizes only: several ffmpeg scalers dislike odd dimensions, and
 * every source in a mosaic is decoded directly to its tile size. */
static int even_floor(int value)
{
    if (value < 2) {
        return 0;
    }
    return value - (value % 2);
}

static size_t rows_for(size_t count, size_t columns)
{
    return (count + columns - 1u) / columns;
}

size_t krtsp_mosaic_columns(
    int canvas_width, int canvas_height, size_t count, float aspect)
{
    size_t best_columns = 1u;
    float best_score = -1.0f;

    if (canvas_width <= 0 || canvas_height <= 0 || count == 0u) {
        return 0u;
    }
    if (!(aspect > 0.0f)) {
        aspect = 16.0f / 9.0f;
    }

    for (size_t columns = 1u; columns <= count; ++columns) {
        size_t rows = rows_for(count, columns);
        float tile_width = (float)canvas_width / (float)columns;
        float tile_height = (float)canvas_height / (float)rows;
        float score;

        if (tile_width < 2.0f || tile_height < 2.0f) {
            continue;
        }
        /* Ratio distance rather than absolute: being 2x too wide and 2x
         * too tall should score the same, which |a-b| does not give. */
        score = (tile_width / tile_height) / aspect;
        if (score < 1.0f) {
            score = 1.0f / score;
        }
        /* Wasted canvas is a real cost too - a 3x3 holding 7 cameras
         * leaves two empty cells - so break ties toward fuller grids. */
        score += 0.05f * (float)(columns * rows - count);

        if (best_score < 0.0f || score < best_score) {
            best_score = score;
            best_columns = columns;
        }
    }
    return best_columns;
}

size_t krtsp_mosaic_layout(
    int canvas_width,
    int canvas_height,
    size_t count,
    float aspect,
    krtsp_tile *tiles,
    size_t capacity)
{
    size_t columns;
    size_t rows;
    int tile_width;
    int tile_height;
    size_t placed = 0u;

    if (tiles == NULL || count == 0u || count > capacity ||
        canvas_width <= 0 || canvas_height <= 0) {
        return 0u;
    }
    columns = krtsp_mosaic_columns(canvas_width, canvas_height, count, aspect);
    if (columns == 0u) {
        return 0u;
    }
    rows = rows_for(count, columns);

    tile_width = even_floor(canvas_width / (int)columns);
    tile_height = even_floor(canvas_height / (int)rows);
    if (tile_width <= 0 || tile_height <= 0) {
        return 0u;
    }

    {
        /* Centre the whole grid, so the rounding lost to even tiles is
         * shared between both margins rather than piling up on one. */
        int grid_width = tile_width * (int)columns;
        int grid_height = tile_height * (int)rows;
        int origin_x = (canvas_width - grid_width) / 2;
        int origin_y = (canvas_height - grid_height) / 2;

        for (size_t index = 0u; index < count; ++index) {
            size_t row = index / columns;
            size_t column = index % columns;
            size_t in_this_row = count - row * columns;
            int row_offset = 0;

            if (in_this_row > columns) {
                in_this_row = columns;
            }
            /* A short final row is centred: left-aligned, three tiles
             * under four read as a missing camera rather than a layout. */
            if (in_this_row < columns) {
                row_offset =
                    (int)((columns - in_this_row) * (size_t)tile_width) / 2;
            }

            tiles[placed].x = origin_x + row_offset +
                              (int)column * tile_width;
            tiles[placed].y = origin_y + (int)row * tile_height;
            tiles[placed].width = tile_width;
            tiles[placed].height = tile_height;
            tiles[placed].index = index;
            placed++;
        }
    }
    return placed;
}
