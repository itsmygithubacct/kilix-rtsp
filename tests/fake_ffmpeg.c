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

    /* Answer -version like the real binary.  krtsp_source probes for the
     * libavformat major to choose between -timeout and -stimeout, and a
     * stand-in that ignored this would stream frames at the probe until
     * it blocked forever.  FAKE_FFMPEG_LIBAVFORMAT selects the reported
     * major so both spellings are reachable. */
    for (int index = 1; index < argc; ++index) {
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
