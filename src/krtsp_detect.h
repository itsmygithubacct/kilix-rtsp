#ifndef KRTSP_DETECT_H
#define KRTSP_DETECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Object detection on the live picture, for the view command.
 *
 * The detector is a subprocess behind a fixed-size pipe, the same contract
 * kilix-object-detect's kilix-look speaks: write one square of BGRA, read
 * back float32[20][6] of [class, score, y0, x0, y1, x1] with coordinates
 * normalised to that square.  kilix-rtsp implements the client side itself
 * rather than linking kilix-object-detect because the dependency runs the
 * other way - kilix-object-detect carries kilix-rtsp as a submodule - and
 * a contract this small is cheaper to speak twice than to invert.
 *
 * The worker runs on its own thread.  A model takes tens of milliseconds
 * per frame on a CPU and up to a minute to load, and none of that is the
 * render loop's to wait for: the loop hands over a frame when the worker is
 * idle and takes whatever result is newest.
 */

#define KRTSP_DETECT_SIZE 320
#define KRTSP_DETECT_ROWS 20
#define KRTSP_DETECT_COLUMNS 6
#define KRTSP_DETECT_REPLY_BYTES \
    (KRTSP_DETECT_ROWS * KRTSP_DETECT_COLUMNS * (int)sizeof(float))
#define KRTSP_DETECT_MAX KRTSP_DETECT_ROWS
#define KRTSP_DETECT_MIN_SCORE 0.35f

typedef struct krtsp_detection {
    int class_id;
    float score;
    int x0;   /* in frame pixels, clipped to the frame */
    int y0;
    int x1;
    int y1;
} krtsp_detection;

/* COCO's eighty class names, or "object" for an id outside them. */
const char *krtsp_detect_class_name(int class_id);

/* Where a frame sits inside the square: scaled to fit, centred. */
typedef struct krtsp_fit {
    float scale;
    int pad_x;
    int pad_y;
    int width;    /* the scaled frame's size inside the square */
    int height;
} krtsp_fit;

krtsp_fit krtsp_detect_fit(int frame_width, int frame_height, int size);

/*
 * Cut the square a detector wants from an RGBA frame: fit, centred, black
 * padding, each output pixel the average of the source pixels it covers.
 * Averaging rather than picking one pixel matters for the small far-away
 * object a camera is usually watching for.  `bgra` holds size*size*4 bytes.
 */
void krtsp_detect_square(
    const uint8_t *rgba, int frame_width, int frame_height, int size,
    uint8_t *bgra);

/*
 * Turn a reply into boxes in frame pixels.  Rows below `min_score`,
 * without a sane class, or with an empty or inverted box are dropped; boxes
 * in the padding are clipped to the picture.  Returns how many were stored.
 */
size_t krtsp_detect_parse(
    const float *reply, int frame_width, int frame_height, int size,
    float min_score, krtsp_detection *out, size_t capacity);

/*
 * The detector command as argv words: KILIX_OBJECT_DETECTOR if set, else
 * the YOLOX runtime `kilix install yolox` writes, else the YOLO one.  The
 * geometry words are appended by the caller.  `storage` holds the strings
 * argv points into.  Returns the number of words, or 0 when none is
 * installed.
 */
size_t krtsp_detect_resolve(
    char *storage, size_t storage_size, const char **argv, size_t capacity);

typedef struct krtsp_detector krtsp_detector;

typedef enum krtsp_detector_state {
    KRTSP_DETECTOR_LOADING = 0,   /* started, no reply yet */
    KRTSP_DETECTOR_RUNNING,
    KRTSP_DETECTOR_FAILED
} krtsp_detector_state;

/* Start the detector process and its worker.  `argv` is NULL-terminated and
 * has no geometry words; they are added.  False when it cannot be spawned. */
bool krtsp_detector_start(krtsp_detector **detector, const char *const *argv);

/* Stop the worker and the process.  Safe on NULL. */
void krtsp_detector_stop(krtsp_detector *detector);

/* Offer a frame.  False when the worker is still busy with the last one, or
 * has failed; the frame is copied, so the caller keeps its buffer. */
bool krtsp_detector_submit(
    krtsp_detector *detector, const uint8_t *rgba, int width, int height);

/* The newest result and the frame size it belongs to.  `generation` rises
 * with each reply so a caller can tell a new answer from the last one. */
size_t krtsp_detector_take(
    krtsp_detector *detector, krtsp_detection *out, size_t capacity,
    uint64_t *generation);

krtsp_detector_state krtsp_detector_status(const krtsp_detector *detector);

/* Why it failed, or an empty string. */
const char *krtsp_detector_error(const krtsp_detector *detector);

#endif
