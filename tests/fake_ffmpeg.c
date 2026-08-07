/*
 * A stand-in for ffmpeg that can be told to misbehave.
 *
 * Process supervision is the part of kilix-rtsp most likely to be written
 * wrong, and the failure that matters most - a camera that stays
 * connected while silently delivering nothing - cannot be reproduced with
 * a real camera on demand.  This binary reproduces all of them on demand,
 * so the supervisor is testable without a camera, a network, or luck.
 *
 * It ignores every argument except the ones below, so krtsp_source can
 * hand it a genuine ffmpeg argv unchanged.  Behavior comes from the
 * environment instead:
 *
 *   FAKE_FFMPEG_WIDTH / _HEIGHT   frame geometry (default 8x4)
 *   FAKE_FFMPEG_FRAMES            frames to emit, -1 = forever (default -1)
 *   FAKE_FFMPEG_DELAY_MS          pause between frames (default 10)
 *   FAKE_FFMPEG_MODE              normal   emit frames, exit 0
 *                                 die      emit FRAMES then exit 1
 *                                 silent   emit FRAMES then sleep forever,
 *                                          holding the pipe open - the
 *                                          wedged-camera case
 *                                 partial  emit FRAMES then half a frame,
 *                                          then keep going - the case that
 *                                          skews every later frame if the
 *                                          reader does not resume a short
 *                                          read
 *                                 garbage  emit FRAMES then a byte count
 *                                          that is not a frame multiple
 *                                 startfail exit 1 immediately
 *   FAKE_FFMPEG_STARTUP_MS        delay before the first frame (default 0),
 *                                 for exercising the startup grace period
 *   FAKE_FFMPEG_LOG               append a line per run, to prove restarts
 *   FAKE_FFMPEG_SEGMENTS          when set, touch the -segment_list path
 *                                 this many times at _SEGMENT_MS spacing
 *                                 and then stop, standing in for a
 *                                 segmenter that wedges while the camera
 *                                 keeps delivering.  -1 keeps rotating.
 *   FAKE_FFMPEG_SEGMENT_MS        spacing between those touches
 *
 * Each frame is filled with a single byte that increments per frame, so a
 * reader can detect both torn frames and lost ones.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (long long)now.tv_sec * 1000 + (long long)now.tv_nsec / 1000000;
}

static int env_int(const char *name, int fallback)
{
    const char *value = getenv(name);
    char *end;
    long parsed;

    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || *end != '\0') {
        return fallback;
    }
    return (int)parsed;
}

static void sleep_ms(int milliseconds)
{
    struct timespec pause;

    if (milliseconds <= 0) {
        return;
    }
    pause.tv_sec = milliseconds / 1000;
    pause.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    (void)nanosleep(&pause, NULL);
}

/* Write every byte or fail: a partial write here would look like the
 * partial-frame fault this program is meant to inject deliberately. */
static bool write_all(const unsigned char *data, size_t size)
{
    size_t written = 0u;

    while (written < size) {
        ssize_t count = write(STDOUT_FILENO, data + written, size - written);

        if (count > 0) {
            written += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    const char *mode = getenv("FAKE_FFMPEG_MODE");
    const char *log_path = getenv("FAKE_FFMPEG_LOG");
    int width = env_int("FAKE_FFMPEG_WIDTH", 8);
    int height = env_int("FAKE_FFMPEG_HEIGHT", 4);
    int frames = env_int("FAKE_FFMPEG_FRAMES", -1);
    int delay_ms = env_int("FAKE_FFMPEG_DELAY_MS", 10);
    int startup_ms = env_int("FAKE_FFMPEG_STARTUP_MS", 0);
    size_t frame_size;
    unsigned char *frame;
    int emitted = 0;
    const char *segment_list = NULL;
    int segments = 0;
    int segment_ms = 200;
    int written = 0;
    long long last_segment = 0;

    /* Answer -version like the real binary.  krtsp_source probes for the
     * libavformat major to choose between -timeout and -stimeout, and a
     * stand-in that ignored this would stream frames at the probe until
     * it blocked forever.  FAKE_FFMPEG_LIBAVFORMAT selects the reported
     * major so both spellings are reachable. */
    for (int index = 1; index < argc; ++index) {
        /* Capability probes must answer and exit, for the same reason
         * -version does: krtsp_source asks the binary what it supports
         * before spawning it, and a stand-in that streamed frames at the
         * question would block the probe forever.  FAKE_FFMPEG_MKDIR
         * selects whether the segment muxer claims -strftime_mkdir, so
         * both branches of that probe are reachable. */
        if (strcmp(argv[index], "-h") == 0 && index + 1 < argc &&
            strncmp(argv[index + 1], "muxer=", 6u) == 0) {
            (void)printf("Segment muxer AVOptions:\n");
            (void)printf("  -strftime          <boolean>    E..........\n");
            if (env_int("FAKE_FFMPEG_MKDIR", 0) != 0) {
                (void)printf("  -strftime_mkdir    <boolean>    E..........\n");
            }
            return 0;
        }
        if (strcmp(argv[index], "-version") == 0) {
            (void)printf("ffmpeg version 6.0-fake\n");
            (void)printf("libavformat    %d. 16.100 / %d. 16.100\n",
                         env_int("FAKE_FFMPEG_LIBAVFORMAT", 60),
                         env_int("FAKE_FFMPEG_LIBAVFORMAT", 60));
            return 0;
        }
    }

    if (mode == NULL) {
        mode = "normal";
    }
    if (log_path != NULL) {
        FILE *log = fopen(log_path, "a");

        if (log != NULL) {
            (void)fprintf(log, "start mode=%s pid=%ld\n", mode, (long)getpid());
            (void)fclose(log);
        }
    }

    if (strcmp(mode, "startfail") == 0) {
        (void)fprintf(stderr, "fake-ffmpeg: refusing to start\n");
        return 1;
    }

    /* Recording stand-in: rewrite the manifest the supervisor stats,
     * then stop, so a wedged segmenter is reproducible on demand the way
     * a wedged camera already is. */
    {
        const char *list = NULL;

        for (int index = 1; index + 1 < argc; ++index) {
            if (strcmp(argv[index], "-segment_list") == 0) {
                list = argv[index + 1];
                break;
            }
        }
        if (list != NULL) {
            segment_list = list;
            segments = env_int("FAKE_FFMPEG_SEGMENTS", 0);
            segment_ms = env_int("FAKE_FFMPEG_SEGMENT_MS", 200);
        }
    }

    if (width <= 0 || height <= 0) {
        return 2;
    }
    frame_size = (size_t)width * (size_t)height * 4u;
    frame = malloc(frame_size);
    if (frame == NULL) {
        return 2;
    }

    sleep_ms(startup_ms);

    while (frames < 0 || emitted < frames) {
        if (segment_list != NULL && (segments < 0 || written < segments)) {
            long long now = now_ms();

            if (now - last_segment >= segment_ms) {
                FILE *manifest = fopen(segment_list, "w");

                if (manifest != NULL) {
                    (void)fprintf(manifest, "segment-%d.mkv\n", written);
                    (void)fclose(manifest);
                }
                last_segment = now;
                written++;
            }
        }
        memset(frame, (unsigned char)((emitted % 254) + 1), frame_size);
        if (!write_all(frame, frame_size)) {
            free(frame);
            return 0;   /* the reader went away; that is not our failure */
        }
        emitted++;
        sleep_ms(delay_ms);
    }

    if (strcmp(mode, "die") == 0) {
        (void)fprintf(stderr, "fake-ffmpeg: exiting after %d frames\n", emitted);
        free(frame);
        return 1;
    }
    if (strcmp(mode, "silent") == 0) {
        /* Hold stdout open and stop producing.  The pipe stays valid, so
         * the reader blocks in read() with no error, forever.  Only a
         * watchdog notices this. */
        free(frame);
        for (;;) {
            sleep_ms(1000);
        }
    }
    if (strcmp(mode, "partial") == 0) {
        /* Each frame is uniform but arrives in two writes with a pause
         * between them, which forces the reader into a short read on
         * every single frame.
         *
         * The content stays uniform on purpose: a reader that resumes the
         * remainder sees clean frames, while one that discards it is
         * misaligned by half a frame from here on and every later frame
         * is visibly a mixture.  Splitting one frame's halves across two
         * different frames instead would make even a correct reader see
         * mixtures, and would test nothing. */
        for (int index = 0; index < 12; ++index) {
            size_t first = frame_size / 2u;

            memset(frame, (unsigned char)((emitted % 254) + 1), frame_size);
            if (!write_all(frame, first)) {
                break;
            }
            sleep_ms(delay_ms);
            if (!write_all(frame + first, frame_size - first)) {
                break;
            }
            emitted++;
            sleep_ms(delay_ms);
        }
        free(frame);
        return 0;
    }
    if (strcmp(mode, "garbage") == 0) {
        memset(frame, 0x5Au, frame_size);
        (void)write_all(frame, frame_size / 3u);
        free(frame);
        return 0;
    }

    free(frame);
    return 0;
}
