#include "krtsp_detect.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

typedef struct test_case {
    const char *name;
    bool (*function)(void);
} test_case;

static bool test_class_names(void)
{
    CHECK(strcmp(krtsp_detect_class_name(0), "person") == 0);
    CHECK(strcmp(krtsp_detect_class_name(2), "car") == 0);
    CHECK(strcmp(krtsp_detect_class_name(16), "dog") == 0);
    CHECK(strcmp(krtsp_detect_class_name(79), "toothbrush") == 0);
    CHECK(strcmp(krtsp_detect_class_name(80), "object") == 0);
    CHECK(strcmp(krtsp_detect_class_name(-1), "object") == 0);
    return true;
}

static bool test_fit_letterboxes_and_centres(void)
{
    krtsp_fit wide = krtsp_detect_fit(1920, 1080, 320);
    krtsp_fit tall = krtsp_detect_fit(1080, 1920, 320);
    krtsp_fit square = krtsp_detect_fit(640, 640, 320);

    CHECK(wide.width == 320 && wide.height == 180);
    CHECK(wide.pad_x == 0 && wide.pad_y == 70);
    CHECK(tall.height == 320 && tall.width == 180);
    CHECK(tall.pad_x == 70 && tall.pad_y == 0);
    CHECK(square.width == 320 && square.pad_x == 0 && square.pad_y == 0);
    /* A frame smaller than the square is scaled up to it, not left tiny. */
    CHECK(krtsp_detect_fit(160, 90, 320).width == 320);
    /* Nonsense in, whole square out: never a division by zero. */
    CHECK(krtsp_detect_fit(0, 0, 320).width == 320);
    return true;
}

static bool test_square_keeps_channels_padding_and_averages(void)
{
    enum { W = 640, H = 360 };
    uint8_t *frame = malloc((size_t)W * H * 4u);
    uint8_t *square = malloc(320u * 320u * 4u);
    krtsp_fit fit = krtsp_detect_fit(W, H, 320);

    CHECK(frame != NULL && square != NULL);
    /* Left half solid RGBA (200, 100, 50); right half black.  Each row is
     * two flat halves, so an area average over a 2x2 block that straddles
     * nothing is exact and one that straddles the seam is not. */
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t *at = frame + ((size_t)y * W + (size_t)x) * 4u;

            at[0] = x < W / 2 ? 200u : 0u;
            at[1] = x < W / 2 ? 100u : 0u;
            at[2] = x < W / 2 ? 50u : 0u;
            at[3] = 255u;
        }
    }
    krtsp_detect_square(frame, W, H, 320, square);

    /* Inside the left half: BGRA order, so byte 0 is blue (50), byte 1
     * green (100), byte 2 red (200), opaque. */
    {
        const uint8_t *at = square + ((size_t)(fit.pad_y + 10) * 320u + 20u) * 4u;

        CHECK(at[0] == 50u && at[1] == 100u && at[2] == 200u && at[3] == 255u);
    }
    /* The padding above and below is opaque black. */
    {
        const uint8_t *top = square + ((size_t)(fit.pad_y - 1) * 320u + 100u) * 4u;
        const uint8_t *bottom =
            square + ((size_t)(fit.pad_y + fit.height) * 320u + 100u) * 4u;

        CHECK(top[0] == 0u && top[1] == 0u && top[2] == 0u && top[3] == 255u);
        CHECK(bottom[0] == 0u && bottom[2] == 0u && bottom[3] == 255u);
    }
    free(frame);
    free(square);
    return true;
}

static bool test_square_averages_rather_than_samples(void)
{
    /* A 4x4 frame of alternating black and white pixels, cut to a 2x2
     * square: nearest-neighbour would return pure black or pure white,
     * an average returns grey.  This is what keeps a thin far-off object
     * visible after the downscale. */
    uint8_t frame[4 * 4 * 4];
    uint8_t square[2 * 2 * 4];

    for (int index = 0; index < 16; ++index) {
        uint8_t value = (uint8_t)(((index % 4) + (index / 4)) % 2 == 0 ? 255 : 0);

        frame[index * 4] = value;
        frame[index * 4 + 1] = value;
        frame[index * 4 + 2] = value;
        frame[index * 4 + 3] = 255u;
    }
    krtsp_detect_square(frame, 4, 4, 2, square);
    for (int pixel = 0; pixel < 4; ++pixel) {
        CHECK(square[pixel * 4] >= 120u && square[pixel * 4] <= 135u);
    }
    return true;
}

static bool test_parse_maps_back_to_frame_pixels(void)
{
    float reply[KRTSP_DETECT_ROWS * KRTSP_DETECT_COLUMNS] = {0};
    krtsp_detection boxes[KRTSP_DETECT_MAX];

    /* 640x360 in a 320 square: scale 0.5, content rows 70..250. */
    reply[0] = 2.0f;          /* car */
    reply[1] = 0.8f;
    reply[2] = 110.0f / 320.0f;   /* y0 -> frame y 80 */
    reply[3] = 40.0f / 320.0f;    /* x0 -> frame x 80 */
    reply[4] = 210.0f / 320.0f;   /* y1 -> frame y 280 */
    reply[5] = 200.0f / 320.0f;   /* x1 -> frame x 400 */
    CHECK(krtsp_detect_parse(reply, 640, 360, 320, 0.35f, boxes, 20) == 1u);
    CHECK(boxes[0].class_id == 2);
    CHECK(boxes[0].x0 == 80 && boxes[0].x1 == 400);
    CHECK(boxes[0].y0 == 80 && boxes[0].y1 == 280);
    return true;
}

static bool test_parse_drops_what_it_should(void)
{
    float reply[KRTSP_DETECT_ROWS * KRTSP_DETECT_COLUMNS] = {0};
    krtsp_detection boxes[KRTSP_DETECT_MAX];

    /* Below the score. */
    reply[0] = 1.0f; reply[1] = 0.2f;
    reply[2] = 0.3f; reply[3] = 0.3f; reply[4] = 0.6f; reply[5] = 0.6f;
    /* Not a number. */
    reply[6] = 1.0f; reply[7] = NAN;
    reply[8] = 0.3f; reply[9] = 0.3f; reply[10] = 0.6f; reply[11] = 0.6f;
    /* Inverted box. */
    reply[12] = 1.0f; reply[13] = 0.9f;
    reply[14] = 0.6f; reply[15] = 0.6f; reply[16] = 0.3f; reply[17] = 0.3f;
    /* Negative class. */
    reply[18] = -1.0f; reply[19] = 0.9f;
    reply[20] = 0.3f; reply[21] = 0.3f; reply[22] = 0.6f; reply[23] = 0.6f;
    /* Entirely inside the padding of a 640x360 frame: clips to nothing. */
    reply[24] = 1.0f; reply[25] = 0.9f;
    reply[26] = 0.0f; reply[27] = 0.3f; reply[28] = 0.1f; reply[29] = 0.6f;
    CHECK(krtsp_detect_parse(reply, 640, 360, 320, 0.35f, boxes, 20) == 0u);

    /* A box that spills into the padding is clipped to the picture. */
    (void)memset(reply, 0, sizeof(reply));
    reply[0] = 0.0f; reply[1] = 0.9f;
    reply[2] = 0.0f; reply[3] = 0.0f; reply[4] = 1.0f; reply[5] = 1.0f;
    CHECK(krtsp_detect_parse(reply, 640, 360, 320, 0.35f, boxes, 20) == 1u);
    CHECK(boxes[0].x0 == 0 && boxes[0].x1 == 640);
    CHECK(boxes[0].y0 == 0 && boxes[0].y1 == 360);
    /* Capacity is honoured. */
    CHECK(krtsp_detect_parse(reply, 640, 360, 320, 0.35f, boxes, 0) == 0u);
    return true;
}

static bool test_resolve_prefers_the_variable_then_yolox_then_yolo(void)
{
    char storage[1024];
    const char *argv[8];
    char dir[256];
    char path[400];
    FILE *file;

    (void)unsetenv("KILIX_OBJECT_DETECTOR");
    (void)snprintf(dir, sizeof(dir), "/tmp/krtsp-detect.XXXXXX");
    CHECK(mkdtemp(dir) != NULL);
    CHECK(setenv("GPU_TERMINAL_DATA_HOME", dir, 1) == 0);

    /* Nothing installed. */
    CHECK(krtsp_detect_resolve(storage, sizeof(storage), argv, 8u) == 0u);

    /* Only the YOLO runtime. */
    (void)snprintf(path, sizeof(path), "%s/runtimes/yolo/bin", dir);
    (void)snprintf(storage, sizeof(storage), "mkdir -p %s", path);
    CHECK(system(storage) == 0);
    (void)snprintf(path, sizeof(path), "%s/runtimes/yolo/bin/kilix-look-detect", dir);
    file = fopen(path, "w");
    CHECK(file != NULL);
    (void)fputs("#!/bin/sh\n", file);
    (void)fclose(file);
    CHECK(chmod(path, 0700) == 0);
    CHECK(krtsp_detect_resolve(storage, sizeof(storage), argv, 8u) == 1u);
    CHECK(strstr(argv[0], "kilix-look-detect") != NULL);

    /* YOLOX wins over it. */
    (void)snprintf(path, sizeof(path), "%s/runtimes/yolox/bin", dir);
    (void)snprintf(storage, sizeof(storage), "mkdir -p %s", path);
    CHECK(system(storage) == 0);
    (void)snprintf(path, sizeof(path), "%s/runtimes/yolox/bin/kilix-yolox-detect", dir);
    file = fopen(path, "w");
    CHECK(file != NULL);
    (void)fputs("#!/bin/sh\n", file);
    (void)fclose(file);
    CHECK(chmod(path, 0700) == 0);
    CHECK(krtsp_detect_resolve(storage, sizeof(storage), argv, 8u) == 1u);
    CHECK(strstr(argv[0], "kilix-yolox-detect") != NULL);

    /* The variable wins over both, and is split on spaces. */
    CHECK(setenv("KILIX_OBJECT_DETECTOR", "ssh gpubox kilix-yolox-detect", 1) == 0);
    CHECK(krtsp_detect_resolve(storage, sizeof(storage), argv, 8u) == 3u);
    CHECK(strcmp(argv[0], "ssh") == 0 && strcmp(argv[2], "kilix-yolox-detect") == 0);
    CHECK(argv[3] == NULL);
    /* A variable naming a path that does not exist is stale: the
     * installed runtime is used instead of failing on it. */
    CHECK(setenv("KILIX_OBJECT_DETECTOR", "/nonexistent/old-home/detect", 1) == 0);
    CHECK(krtsp_detect_resolve(storage, sizeof(storage), argv, 8u) == 1u);
    CHECK(strstr(argv[0], "kilix-yolox-detect") != NULL);
    CHECK(setenv("KILIX_OBJECT_DETECTOR", "ssh gpubox kilix-yolox-detect", 1) == 0);
    /* Too many words is refused whole, not truncated. */
    CHECK(krtsp_detect_resolve(storage, sizeof(storage), argv, 3u) == 0u);

    (void)unsetenv("KILIX_OBJECT_DETECTOR");
    (void)unsetenv("GPU_TERMINAL_DATA_HOME");
    (void)snprintf(storage, sizeof(storage), "rm -rf %s", dir);
    CHECK(system(storage) == 0);
    return true;
}

static const char *fake(void)
{
    const char *path = getenv("KRTSP_FAKE_DETECTOR");

    return path != NULL ? path : "build/fake-detector";
}

static bool wait_for(krtsp_detector *detector, uint64_t generation)
{
    krtsp_detection boxes[KRTSP_DETECT_MAX];

    for (int attempt = 0; attempt < 200; ++attempt) {
        uint64_t seen = 0u;

        (void)krtsp_detector_take(detector, boxes, KRTSP_DETECT_MAX, &seen);
        if (seen >= generation ||
            krtsp_detector_status(detector) == KRTSP_DETECTOR_FAILED) {
            return seen >= generation;
        }
        {
            struct timespec pause = {0, 25 * 1000 * 1000};

            (void)nanosleep(&pause, NULL);
        }
    }
    return false;
}

static bool test_a_frame_goes_through_the_pipe_and_back(void)
{
    const char *argv[] = {fake(), NULL};
    krtsp_detector *detector = NULL;
    krtsp_detection boxes[KRTSP_DETECT_MAX];
    uint8_t frame[64 * 36 * 4];
    uint64_t generation = 0u;

    CHECK(krtsp_detector_start(&detector, argv));
    CHECK(krtsp_detector_status(detector) == KRTSP_DETECTOR_LOADING);
    /* RGBA solid red = 30: the fake reports class red/3 = 10. */
    for (size_t pixel = 0u; pixel < sizeof(frame) / 4u; ++pixel) {
        frame[pixel * 4u] = 30u;
        frame[pixel * 4u + 1u] = 0u;
        frame[pixel * 4u + 2u] = 0u;
        frame[pixel * 4u + 3u] = 255u;
    }
    CHECK(krtsp_detector_submit(detector, frame, 64, 36));
    CHECK(wait_for(detector, 1u));
    CHECK(krtsp_detector_status(detector) == KRTSP_DETECTOR_RUNNING);
    CHECK(krtsp_detector_take(detector, boxes, KRTSP_DETECT_MAX,
                              &generation) == 1u);
    CHECK(generation == 1u);
    CHECK(boxes[0].class_id == 10);
    /* Middle of a 64x36 frame: [0.25, 0.75] of the square, less padding. */
    CHECK(boxes[0].x0 == 16 && boxes[0].x1 == 48);
    CHECK(boxes[0].y0 > 0 && boxes[0].y1 < 36 && boxes[0].y1 > boxes[0].y0);

    /* Each reply is a new generation and the worker takes the next frame. */
    CHECK(krtsp_detector_submit(detector, frame, 64, 36));
    CHECK(wait_for(detector, 2u));
    krtsp_detector_stop(detector);
    return true;
}

static bool test_a_busy_worker_refuses_a_second_frame(void)
{
    const char *argv[] = {fake(), NULL};
    krtsp_detector *detector = NULL;
    uint8_t frame[16 * 16 * 4] = {0};
    bool refused = false;

    CHECK(krtsp_detector_start(&detector, argv));
    CHECK(krtsp_detector_submit(detector, frame, 16, 16));
    for (int attempt = 0; attempt < 50 && !refused; ++attempt) {
        refused = !krtsp_detector_submit(detector, frame, 16, 16);
    }
    /* The worker is either still on the first frame or has just finished
     * it; a hundred offers in a tight loop cannot all be accepted. */
    CHECK(refused);
    krtsp_detector_stop(detector);
    return true;
}

static bool test_a_detector_that_exits_is_reported_not_hung_on(void)
{
    const char *argv[] = {fake(), NULL};
    krtsp_detector *detector = NULL;
    uint8_t frame[16 * 16 * 4] = {0};

    CHECK(setenv("FAKE_DETECTOR_MODE", "exit", 1) == 0);
    CHECK(krtsp_detector_start(&detector, argv));
    CHECK(krtsp_detector_submit(detector, frame, 16, 16));
    CHECK(!wait_for(detector, 1u));
    CHECK(krtsp_detector_status(detector) == KRTSP_DETECTOR_FAILED);
    CHECK(strstr(krtsp_detector_error(detector), "exited") != NULL);
    /* A failed detector accepts nothing more. */
    CHECK(!krtsp_detector_submit(detector, frame, 16, 16));
    krtsp_detector_stop(detector);
    (void)unsetenv("FAKE_DETECTOR_MODE");
    return true;
}

static bool test_garbage_in_a_reply_is_dropped_not_drawn(void)
{
    const char *argv[] = {fake(), NULL};
    krtsp_detector *detector = NULL;
    krtsp_detection boxes[KRTSP_DETECT_MAX];
    uint8_t frame[16 * 16 * 4] = {0};
    uint64_t generation = 0u;

    CHECK(setenv("FAKE_DETECTOR_MODE", "garbage", 1) == 0);
    CHECK(krtsp_detector_start(&detector, argv));
    CHECK(krtsp_detector_submit(detector, frame, 16, 16));
    CHECK(wait_for(detector, 1u));
    /* Row 0 is good; the NaN row and the inverted box are gone. */
    CHECK(krtsp_detector_take(detector, boxes, KRTSP_DETECT_MAX,
                              &generation) == 1u);
    krtsp_detector_stop(detector);
    (void)unsetenv("FAKE_DETECTOR_MODE");
    return true;
}

static bool test_a_missing_command_does_not_start(void)
{
    const char *argv[] = {"/nonexistent/krtsp-detector", NULL};
    krtsp_detector *detector = NULL;

    /* posix_spawnp may report the failure at spawn or the child may die at
     * once; either way nothing is left running and stop is safe. */
    if (krtsp_detector_start(&detector, argv)) {
        uint8_t frame[8 * 8 * 4] = {0};

        (void)krtsp_detector_submit(detector, frame, 8, 8);
        (void)wait_for(detector, 1u);
        CHECK(krtsp_detector_status(detector) == KRTSP_DETECTOR_FAILED);
    }
    krtsp_detector_stop(detector);
    CHECK(!krtsp_detector_start(&detector, (const char *const[]){NULL}));
    return true;
}

int
main(void)
{
    static const test_case tests[] = {
        {"class names", test_class_names},
        {"fit letterboxes and centres", test_fit_letterboxes_and_centres},
        {"square keeps channels and padding",
         test_square_keeps_channels_padding_and_averages},
        {"square averages rather than samples",
         test_square_averages_rather_than_samples},
        {"parse maps back to frame pixels",
         test_parse_maps_back_to_frame_pixels},
        {"parse drops what it should", test_parse_drops_what_it_should},
        {"resolve prefers the variable, then yolox, then yolo",
         test_resolve_prefers_the_variable_then_yolox_then_yolo},
        {"a frame goes through the pipe and back",
         test_a_frame_goes_through_the_pipe_and_back},
        {"a busy worker refuses a second frame",
         test_a_busy_worker_refuses_a_second_frame},
        {"a detector that exits is reported, not hung on",
         test_a_detector_that_exits_is_reported_not_hung_on},
        {"garbage in a reply is dropped, not drawn",
         test_garbage_in_a_reply_is_dropped_not_drawn},
        {"a missing command does not start",
         test_a_missing_command_does_not_start}
    };
    size_t passed = 0u;

    (void)signal(SIGPIPE, SIG_IGN);
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
