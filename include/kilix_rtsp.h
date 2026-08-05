#ifndef KILIX_RTSP_H
#define KILIX_RTSP_H

/*
 * kilix-rtsp presents RTSP camera streams inside a terminal through the
 * Kitty graphics protocol.
 *
 * Acquisition is an ffmpeg subprocess per stream: the library builds the
 * argument list, spawns the process, and reads fixed-size RGBA frames
 * from its stdout.  It never links libavcodec.  That keeps codec bugs and
 * camera-specific parsing failures inside a separate, restartable process,
 * picks up whatever hardware acceleration the installed ffmpeg has, and
 * makes a fixed pixel format the pipe's framing: read exactly
 * width * height * 4 bytes and you have exactly one frame.
 *
 * The library is acquisition and presentation only.  Recording, retention
 * and object detection belong to whatever consumes it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KILIX_RTSP_VERSION_MAJOR 0
#define KILIX_RTSP_VERSION_MINOR 1
#define KILIX_RTSP_VERSION_PATCH 0

/* -------------------------- argument building --------------------------- */

/*
 * Which of a camera's streams to pull.  Every camera on a typical install
 * publishes at least two, and the right one depends on how big the result
 * will be drawn: a mosaic tile wants the substream, because scaling a
 * 640x360 source into a 320x180 tile already discards half the pixels,
 * while a view filling the terminal wants the main stream rather than
 * upscaling a substream that the camera is already encoding at higher
 * resolution.
 *
 * Tier is therefore a property of the request, not of the camera.
 */
typedef enum krtsp_tier {
    KRTSP_TIER_SUB = 0,   /* low resolution; the mosaic default */
    KRTSP_TIER_MAIN       /* full resolution; the view default.  May be HEVC */
} krtsp_tier;

/*
 * Output pixel order.  This is not cosmetic: getting it wrong swaps red
 * and blue, and the correct answer depends on the consumer.
 *
 * RGBA is byte order R,G,B,A, which is what kittyfb_present() takes.
 * BGRA is byte order B,G,R,A, which on a little-endian machine is the
 * memory layout of soft-raster's 0xAARRGGBB canvas - so anything passing
 * through sr_canvas_wrap() must ask for BGRA.
 */
typedef enum krtsp_pixfmt {
    KRTSP_PIXFMT_RGBA = 0,   /* straight to kittyfb_present() */
    KRTSP_PIXFMT_BGRA        /* straight into an sr_canvas on little-endian */
} krtsp_pixfmt;

typedef struct krtsp_args_request {
    /* Full RTSP URL, credentials included.  See the note on secrets in
     * krtsp_url_redact(). */
    const char *url;

    /* Decoded output size.  Both zero keeps the source size, in which
     * case the caller must discover it before sizing frame buffers. */
    int width;
    int height;

    /* Cap the delivered frame rate; 0 keeps the source rate.  Cameras
     * misreport their rate (one on the reference install advertises 120
     * and delivers 15), so this bounds work rather than describing it. */
    int fps_cap;

    /* Add -fflags nobuffer -flags low_delay.  Correct for display, where
     * latency is the point; wrong for recording, where buffering is free
     * and integrity matters more. */
    bool low_latency;

    krtsp_pixfmt pixfmt;

    /* ffmpeg renamed the RTSP socket timeout: -stimeout below libavformat
     * 59, -timeout from 59 on.  Default false selects -timeout.
     * krtsp_source probes the binary once and sets this. */
    bool legacy_timeout_flag;
} krtsp_args_request;

/* Bounds for krtsp_build_argv() callers sizing their own storage. */
#define KRTSP_ARGV_MAX 40
#define KRTSP_ARGV_STORAGE_MAX 1024

void krtsp_args_request_init(krtsp_args_request *request);

/*
 * Build a NULL-terminated argv for one stream into caller-owned storage.
 * Returns the argument count excluding the NULL terminator, or 0 when the
 * request is invalid or either capacity is insufficient.
 *
 * argv entries point into storage; both must outlive the spawn.  Nothing
 * is allocated.
 */
size_t krtsp_build_argv(
    const krtsp_args_request *request,
    char **argv,
    size_t argv_capacity,
    char *storage,
    size_t storage_capacity);

/*
 * Copy `url` into `out` with any password replaced, for logs and error
 * messages.  An RTSP URL embeds credentials as rtsp://user:password@host,
 * so a URL is a secret and must never reach a log file, an error string,
 * or a crash report unredacted.  Returns false when the output is too
 * small, in which case *out is set to the empty string rather than a
 * truncated - and possibly still revealing - prefix.
 */
bool krtsp_url_redact(const char *url, char *out, size_t capacity);

/* ---------------------------- frame handoff ----------------------------- */

/*
 * The handoff between a source's reader thread and the thread that
 * presents.  One slot: a new frame replaces an unread one rather than
 * queueing behind it.
 *
 * That is the right trade for video and the wrong one for a file: a
 * queue would preserve every frame at the cost of latency, and latency
 * is the only thing a live camera view is for.  It mirrors the policy
 * kitty-framebuffer already applies to its own pending slot.
 *
 * Three buffers: the producer owns one outright, the consumer owns one
 * outright, and a single shared slot is exchanged under the lock.  Two
 * buffers cannot be made safe here - the producer necessarily holds its
 * buffer pointer across the lock while it fills the frame, so any
 * consumer-side swap rewrites that pointer under it.
 *
 * The lock covers two pointer exchanges and some counters, and is never
 * held across a read from the camera or a write to the terminal, which
 * would couple them and let a slow terminal stall capture.
 */
typedef struct krtsp_frame krtsp_frame;

/* Allocate all three buffers at width * height * 4 bytes. */
bool krtsp_frame_init(krtsp_frame **frame, int width, int height);
void krtsp_frame_free(krtsp_frame *frame);

/*
 * Producer: where to write the next frame.
 *
 * Call this again after every publish.  Publishing exchanges the
 * producer's buffer with the shared slot, so a pointer cached across a
 * publish refers to a frame the consumer may now be reading.  This is
 * the one rule the caller has to follow.
 */
uint8_t *krtsp_frame_back(krtsp_frame *frame);
size_t krtsp_frame_size(const krtsp_frame *frame);

/* Producer: publish what was written into the back buffer.  Never blocks
 * on a consumer.  `dropped` receives true when this replaced a frame the
 * consumer never took. */
void krtsp_frame_publish(krtsp_frame *frame, bool *dropped);

/*
 * Consumer: borrow the newest frame, or NULL when none has arrived.
 * `sequence` receives a counter that increments once per published
 * frame, so a caller can tell a new frame from a repeat without
 * comparing pixels.  `age_ms` receives how long ago it was published -
 * the only way to distinguish a frozen camera from a still scene, since
 * both deliver the same bytes forever.
 *
 * Every successful borrow must be released.
 */
const uint8_t *krtsp_frame_borrow(
    krtsp_frame *frame, uint64_t *sequence, int *age_ms);
void krtsp_frame_release(krtsp_frame *frame);

/* Total frames published and frames replaced before being taken. */
void krtsp_frame_stats(
    const krtsp_frame *frame, uint64_t *published, uint64_t *dropped);

/* ------------------------------- sources -------------------------------- */

/*
 * One camera, as a supervised ffmpeg subprocess.
 *
 * Cameras fail constantly, and in more than one way.  The obvious failure
 * is the process dying; the one that actually costs you a night of
 * footage is the camera wedging while holding the TCP connection open, so
 * the socket stays valid, read() blocks forever, and nothing below the
 * application layer reports anything at all.  Those are different states
 * and the supervisor distinguishes them.
 */
typedef enum krtsp_status {
    KRTSP_STARTING = 0,  /* spawned, still inside the startup grace period */
    KRTSP_ONLINE,        /* frames arriving */
    KRTSP_STALE,         /* process alive, no frame within stall_ms */
    KRTSP_OFFLINE,       /* process gone; waiting out the backoff */
    KRTSP_FAILED         /* gave up after max_consecutive_failures */
} krtsp_status;

typedef struct krtsp_source krtsp_source;

typedef struct krtsp_source_options {
    /* Decoded frame size.  Required: frame buffers are sized from it and
     * a fixed size is what makes the pipe self-framing. */
    int width;
    int height;

    int fps_cap;
    krtsp_pixfmt pixfmt;
    bool low_latency;

    /* No frame for this long, after the grace period, means the camera
     * has wedged: kill and restart.  Default 20000. */
    int stall_ms;

    /* Startup allowance before stall_ms applies.  A camera that takes
     * eight seconds to negotiate would otherwise be killed forever.
     * Default 15000. */
    int grace_ms;

    /* Restart backoff, doubling per consecutive failure.  Defaults 1000
     * and 60000. */
    int backoff_min_ms;
    int backoff_max_ms;

    /* Sustained online time that resets the backoff.  Resetting on the
     * first frame instead would let a camera that delivers one frame and
     * dies spin in a tight restart loop.  Default 30000. */
    int stable_ms;

    /* 0 = retry forever, which is what a wall display wants.  Default 0. */
    int max_consecutive_failures;

    /* ffmpeg stderr sink.  NULL discards.  Never inherit the terminal's
     * stderr: an ffmpeg warning printed into the alternate screen
     * corrupts the display, and a flaky camera produces plenty. */
    const char *log_path;

    /* ffmpeg binary.  NULL resolves KILIX_RTSP_FFMPEG, then PATH.  The
     * override matters on hosts carrying a vendor build with different
     * codec support than the distribution's. */
    const char *ffmpeg_path;
} krtsp_source_options;

void krtsp_source_options_init(krtsp_source_options *options);

/*
 * Spawn and supervise.  Returns false only on bad arguments or resource
 * exhaustion: a camera that is unreachable right now is not a failure to
 * start, it is a source that reports KRTSP_OFFLINE and keeps retrying.
 */
bool krtsp_source_start(
    krtsp_source **source,
    const char *url,
    const krtsp_source_options *options);

/* Stop supervision, terminate the child, and release everything.  Safe on
 * NULL and safe to call while the camera is wedged. */
void krtsp_source_stop(krtsp_source *source);

/*
 * Borrow the newest frame, or NULL when none has arrived yet.  `age_ms`
 * receives its age: a wedged camera keeps a valid frame available
 * forever, so age is the only thing that distinguishes it from a still
 * scene.  Release every successful borrow.
 */
const uint8_t *krtsp_source_borrow(krtsp_source *source, int *age_ms);
void krtsp_source_release(krtsp_source *source);

krtsp_status krtsp_source_status(const krtsp_source *source);
const char *krtsp_status_name(krtsp_status status);

typedef struct krtsp_source_stats {
    uint64_t frames;        /* frames read from the pipe */
    uint64_t dropped;       /* published over before being taken */
    uint64_t restarts;      /* child processes spawned after the first */
    uint64_t stalls;        /* restarts caused by silence, not by exit */
    int consecutive_failures;
} krtsp_source_stats;

void krtsp_source_get_stats(
    const krtsp_source *source, krtsp_source_stats *out);

/*
 * True when this build of ffmpeg wants the pre-libavformat-59 spelling of
 * the RTSP socket timeout.  Probed once per process by running the binary;
 * krtsp_source_start() applies it automatically.
 */
bool krtsp_ffmpeg_needs_legacy_timeout(const char *ffmpeg_path);

#ifdef __cplusplus
}
#endif

#endif
