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
 * The library owns acquisition and optional packet-copy recording.  The
 * command adds terminal presentation.  Retention policy and object detection
 * belong to whatever consumes it.
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

/*
 * What one ffmpeg process is being asked to do with the stream.
 *
 * A bitmask rather than a mode, because all three combinations are real:
 * a camera that only feeds detection and a live view, one that only
 * archives, and one that does both.  The last is the reason this is not
 * two processes - many devices refuse a second concurrent RTSP session,
 * and decoding is expensive enough that doing it twice to serve two
 * consumers is the largest avoidable cost in the system.
 *
 * KRTSP_ROLE_RECORD alone never decodes at all: the segmenter copies the
 * camera's own bitstream to disk, so the process costs I/O and nothing
 * else.  That is the cheapest camera this library can run.
 */
typedef enum krtsp_role {
    KRTSP_ROLE_DECODE = 1 << 0,   /* rawvideo to the pipe (the default) */
    KRTSP_ROLE_RECORD = 1 << 1    /* -c copy segments to a directory */
} krtsp_role;

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

    /*
     * Scale to fit inside width x height preserving aspect, then pad to
     * exactly that size with black bars.
     *
     * Worth doing in ffmpeg rather than afterwards: it means every frame
     * arrives at exactly the size the presenter wants, so the read stays
     * fixed-size and self-framing and no software scaling is needed - and
     * it removes the need to know the camera's aspect ratio at all, which
     * otherwise has to be probed before the first frame.
     */
    bool letterbox;

    krtsp_pixfmt pixfmt;

    /* ffmpeg renamed the RTSP socket timeout: -stimeout below libavformat
     * 59, -timeout from 59 on.  Default false selects -timeout.
     * krtsp_source probes the binary once and sets this. */
    bool legacy_timeout_flag;

    /*
     * Pace a local input at its own frame rate (-re).  Ignored for RTSP,
     * which is already paced by the camera.  Default true.
     *
     * A recording read as fast as the disk allows is not a stand-in for a
     * camera: it fills the ring in a second and every consumer sees only
     * the end of it.  Set false when the point is to get through the file
     * quickly - scanning footage for what is in it rather than watching
     * it.
     */
    bool realtime;

    /* Bitmask of krtsp_role.  Zero means KRTSP_ROLE_DECODE, which is what
     * every caller predating the record sink asked for. */
    unsigned roles;

    /*
     * KRTSP_ROLE_RECORD only.  Segments land at record_dir/record_pattern,
     * where the pattern is strftime and its extension picks the container.
     *
     * The container matters more than it looks.  Cameras commonly carry
     * pcm_alaw audio, which mp4 cannot mux at all - `-c copy` fails
     * outright - while MPEG-TS accepts it and silently drops the audio
     * stream.  Matroska carries it untouched and tolerates a segment
     * truncated by a power cut, which mp4 does not.  Hence the .mkv
     * default.
     *
     * NULL pattern means "%Y-%m-%d_%H.%M.%S.mkv" - flat, because that
     * works on every ffmpeg.  A pattern containing '/' builds a date
     * hierarchy instead, which is nicer to prune and to browse, but needs
     * the muxer to create directories: see segment_mkdir.
     *
     * segment_seconds <= 0 means 10.
     */
    const char *record_dir;
    const char *record_pattern;
    int segment_seconds;

    /*
     * Copy the audio track as well as video.  Off by default: it is only
     * safe once the container can carry whatever codec the camera uses,
     * and a wrong pairing fails the whole process rather than the audio.
     * The map is optional, so a camera without audio still records.
     */
    bool record_audio;

    /* Emit -strftime_mkdir, so a pattern containing '/' creates its
     * directories.  Not universal - ffmpeg 5.1 lacks it - so this must be
     * set from krtsp_ffmpeg_supports_segment_mkdir() rather than assumed.
     * krtsp_source probes once and sets it. */
    bool segment_mkdir;

    /*
     * Optional manifest ffmpeg rewrites as each segment completes.
     *
     * It exists so a supervisor can tell whether recording is still
     * happening by stat'ing one file, instead of walking a directory tree
     * that may hold a hundred thousand segments.  Bounded to the most
     * recent few entries; it is a liveness signal, not an index.
     */
    const char *segment_list;
} krtsp_args_request;

/* Bounds for krtsp_build_argv() callers sizing their own storage.
 * Storage allows for password escaping, which can triple the length of
 * the userinfo, on top of the 512-byte URL a source accepts. */
#define KRTSP_ARGV_MAX 64
#define KRTSP_ARGV_STORAGE_MAX 2048

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

/*
 * Percent-encode reserved characters in the URL's password so ffmpeg
 * parses the URL the way the operator meant it.
 *
 * Camera passwords are generated by camera vendors and by people, and
 * they contain '@', '/', '?', '#' and ':' often enough to matter.  Every
 * one of those is structural in a URL: a password containing '@' ends the
 * userinfo early, and ffmpeg then tries to connect to a host made of the
 * rest of the password.  The failure is a connection error naming a host
 * that does not exist, which reads as a network problem rather than as a
 * quoting problem.
 *
 * Only the password is touched.  krtsp_build_argv() applies this to the
 * -i operand automatically, so callers do not have to remember; it is
 * exposed for tests and for anyone building a command by hand.
 */
bool krtsp_url_escape_password(const char *url, char *out, size_t capacity);

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
 * The buffers are a ring of slots addressed by index.  Sizing is forced:
 *
 *     1 slot the producer is filling
 *   + 1 slot holding the newest published frame
 *   + 1 slot per reader currently holding a borrow
 *
 * Two buffers cannot be made safe at all - the producer necessarily holds
 * its buffer across the lock while it fills the frame, so a consumer-side
 * exchange rewrites it underneath.  Three is the minimum, and is what a
 * private ring uses.
 *
 * The lock covers index and counter updates only, and is never held
 * across a read from the camera or a write to the terminal, which would
 * couple them and let a slow reader stall capture.
 */
/* Shared-ring object names: the prefix keeps ours out of every other
 * program's flat POSIX shm namespace and makes leaked objects
 * identifiable in /dev/shm. */
#define KRTSP_FRAME_NAME_PREFIX "/kilix-rtsp-"
#define KRTSP_FRAME_NAME_MAX 96

typedef struct krtsp_frame krtsp_frame;

/* Private ring: three slots of width * height * 4 bytes, this process
 * only.  Correct for a single viewer, and the cheapest option. */
bool krtsp_frame_init(krtsp_frame **frame, int width, int height);

/*
 * Shared ring: the same policy in a POSIX shared-memory object, so other
 * processes can read the frames this one decodes.
 *
 * Decoding is the most expensive thing in this library - a full-screen
 * source runs 21-44% of a core - so a second consumer that decodes its
 * own copy doubles the largest cost in the system to produce bytes that
 * already exist.  Two viewers on one camera do exactly that today.  It
 * also keeps the camera to a single RTSP session, which matters because
 * some devices refuse concurrent ones.
 *
 * `name` is a leaf, not a path: it is prefixed to form the object name,
 * and must not contain '/', a backslash, or whitespace.  One producer
 * owns a given name.  A second live producer is refused; an object whose
 * producer died is recognized by its released lifetime lock and replaced.
 *
 * `max_readers` is how many consumers may hold a borrow *at the same
 * time*, not how many may attach - readers that borrow and release
 * promptly share far fewer slots than their number.  It sizes the ring at
 * max_readers + 2, and krtsp_frame_borrow() refuses beyond it rather than
 * overwriting a slot somebody is reading.
 *
 * Borrowers have process leases, so a reader killed before release does not
 * permanently consume capacity.  The process-shared mutex is robust on
 * Linux and repairs its derived pin state if a holder dies mid-update.
 *
 * Frames are not authenticated.  Anything able to open the object can read
 * the camera's pixels, so the object is forced to exact mode 0600 even under
 * a stricter umask, and the same reasoning that keeps camera URLs out of
 * world-readable files applies.
 */
bool krtsp_frame_init_shared(krtsp_frame **frame, const char *name,
                             int width, int height, int max_readers);

/*
 * Attach to a ring some other process created.  Fails when the object is
 * missing, its producer is no longer alive, its owner/mode/size is unsafe,
 * it was built by an incompatible version, or it is not a ring at all.
 *
 * A reader gets the same borrow/release contract as an in-process
 * consumer.  It never unlinks the object: the producer owns that.
 */
bool krtsp_frame_attach(krtsp_frame **frame, const char *name);

/* The full object name of a shared ring, or NULL for a private one. */
const char *krtsp_frame_name(const krtsp_frame *frame);

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

    /* Pace a local input at its own frame rate (-re); ignored for RTSP.
     * Default true, so a recording stands in for a camera; false to get
     * through a file as fast as it will decode. */
    bool realtime;

    /* Scale-to-fit and pad, so frames arrive at exactly width x height
     * whatever the camera's aspect ratio is.  See krtsp_args_request. */
    bool letterbox;

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

    /*
     * Bitmask of krtsp_role; zero means KRTSP_ROLE_DECODE.  With
     * KRTSP_ROLE_RECORD the same process also writes -c copy segments,
     * which is how one RTSP session serves both a viewer and an archive.
     *
     * KRTSP_ROLE_RECORD without KRTSP_ROLE_DECODE never decodes, so
     * krtsp_frame_borrow() will never return anything and the source
     * costs only I/O.
     */
    unsigned roles;

    /* Recording target; see the matching fields on krtsp_args_request.
     * record_dir is required whenever KRTSP_ROLE_RECORD is set.  The
     * segment_mkdir capability is probed from the binary, so a
     * hierarchical pattern works where the binary supports it. */
    const char *record_dir;
    const char *record_pattern;
    int segment_seconds;
    bool record_audio;

    /*
     * No new segment for this long, after the grace period, means the
     * recording sink has wedged: kill and restart.
     *
     * Separate from stall_ms because the two failures are genuinely
     * independent - a camera can deliver frames while its segmenter is
     * stuck on a full disk, and can write segments while the decode pipe
     * has stopped.  Collapsing them into one timer loses the ability to
     * say which broke.
     *
     * Default 0 selects three segment lengths, which tolerates one missed
     * rotation without tolerating a dead sink.
     */
    int segment_stall_ms;
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

/*
 * Overall health: the more severe of the roles in use, so a source whose
 * segmenter has wedged does not report "online" because pixels are still
 * arriving.  For a decode-only source this is exactly what it always was.
 */
krtsp_status krtsp_source_status(const krtsp_source *source);

/* Health of one role.  A role the source is not running reports
 * KRTSP_OFFLINE rather than pretending to be healthy. */
krtsp_status krtsp_source_role_status(
    const krtsp_source *source, krtsp_role role);
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

/*
 * Whether this ffmpeg's segment muxer can create the directories a
 * strftime pattern names (-strftime_mkdir).  ffmpeg 5.1 cannot.
 *
 * A recording pattern containing '/' needs this, and without it every
 * segment fails once the hour rolls over - silently, until an entire
 * hour of footage is missing.  krtsp_source probes once and passes the
 * flag only when it is real; a caller choosing a hierarchical pattern
 * should check the same thing rather than discover it overnight.
 */
bool krtsp_ffmpeg_supports_segment_mkdir(const char *ffmpeg_path);

/* ------------------------- paths and configuration ---------------------- */

/*
 * Resolve a path inside the private state directory,
 * ~/.local/gpu_terminal/kilix-rtsp, overridable with KILIX_RTSP_HOME.
 *
 * `leaf` names a subdirectory ("config", "data", "cache", "state",
 * "logs") or NULL for the root.  The directory is created if absent,
 * mode 0700, along with the root.
 *
 * Nothing the program reads or writes lives in the repository: the work
 * tree stays free of uncommittable files, and camera configuration - which
 * embeds credentials - never sits where a stray `git add -A` could take it.
 */
bool krtsp_paths_dir(const char *leaf, char *out, size_t capacity);

#define KRTSP_NAME_MAX 64
#define KRTSP_URL_MAX 512
#define KRTSP_CAMERAS_MAX 32
#define KRTSP_GROUP_MEMBERS_MAX 32

typedef struct krtsp_camera {
    char name[KRTSP_NAME_MAX];
    char url_main[KRTSP_URL_MAX];
    char url_sub[KRTSP_URL_MAX];
} krtsp_camera;

typedef struct krtsp_group {
    char name[KRTSP_NAME_MAX];
    char members[KRTSP_GROUP_MEMBERS_MAX][KRTSP_NAME_MAX];
    size_t member_count;
} krtsp_group;

typedef struct krtsp_config krtsp_config;

/*
 * Load camera definitions.  `path` NULL reads
 * <state dir>/config/cameras.conf.
 *
 * The file holds passwords, so it is required to be a regular, non-symlink
 * file owned by the caller with no group or world permission bits.  The
 * opened descriptor itself is checked, so a path replacement cannot swap a
 * different file in after validation.  A readable-by-others credential file
 * is refused rather than warned about: the whole point of keeping it outside
 * the repository is that it is a secret.
 *
 * On failure `error` receives a message that never contains a URL.
 */
bool krtsp_config_load(
    krtsp_config **config, const char *path, char *error, size_t error_capacity);
void krtsp_config_free(krtsp_config *config);

size_t krtsp_config_camera_count(const krtsp_config *config);
const krtsp_camera *krtsp_config_camera_at(
    const krtsp_config *config, size_t index);
const krtsp_camera *krtsp_config_find(
    const krtsp_config *config, const char *name);

size_t krtsp_config_group_count(const krtsp_config *config);
const krtsp_group *krtsp_config_group_at(
    const krtsp_config *config, size_t index);
const krtsp_group *krtsp_config_find_group(
    const krtsp_config *config, const char *name);

/*
 * The URL for a tier, falling back to the other when the requested one is
 * absent, so a camera defined with only a substream still works in both
 * views.  Returns NULL when the camera has no URL at all.
 */
const char *krtsp_camera_url(const krtsp_camera *camera, krtsp_tier tier);

/* ------------------------------- mosaic --------------------------------- */

/*
 * Grid layout for N cameras on one canvas.
 *
 * Pure arithmetic: no canvas, no sources, no terminal.  Layout is where a
 * mosaic is right or wrong - tiles that overlap, leave the canvas, or
 * distort the picture are all layout bugs - so it is kept testable on its
 * own.
 */
typedef struct krtsp_tile {
    int x;
    int y;
    int width;
    int height;
    size_t index;   /* which source this tile belongs to */
} krtsp_tile;

/*
 * Fill `tiles` with a grid for `count` sources on a canvas_width x
 * canvas_height canvas, preferring tiles close to `aspect` (width /
 * height; pass 16.0f/9.0f for typical cameras).
 *
 * Returns the number of tiles placed, or 0 on invalid arguments or
 * insufficient capacity.  Tiles never overlap and never leave the canvas.
 * A final short row is centred, because a row of three under a row of
 * four looks like a mistake when it is left-aligned.
 *
 * Tile dimensions are forced even: odd sizes make some ffmpeg scalers
 * unhappy and every source here is decoded straight to its tile size.
 */
size_t krtsp_mosaic_layout(
    int canvas_width,
    int canvas_height,
    size_t count,
    float aspect,
    krtsp_tile *tiles,
    size_t capacity);

/* Columns the layout would choose; exposed for tests and diagnostics. */
size_t krtsp_mosaic_columns(
    int canvas_width, int canvas_height, size_t count, float aspect);

#ifdef __cplusplus
}
#endif

#endif
