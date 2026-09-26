/*
 * A stand-in detector speaking kilix-look's pipe contract, so the client
 * can be tested without a model.  Reads `--geometry WxH`, then for every
 * square of BGRA it is sent, writes one reply of float32[20][6].
 *
 * The reply is a function of the input, which is the point: row 0 is a box
 * over the middle of the square whose class is the RED byte of the centre
 * pixel divided by three, so a test that sends a red frame and gets the
 * matching class back has proved the channel order end to end.
 *
 *   FAKE_DETECTOR_MODE=exit    exit after the first request, unanswered
 *   FAKE_DETECTOR_MODE=garbage reply with a NaN row and an inverted box
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int width = 0;
    int height = 0;
    const char *mode = getenv("FAKE_DETECTOR_MODE");
    unsigned char *square;
    size_t size;

    for (int index = 1; index + 1 < argc; ++index) {
        if (strcmp(argv[index], "--geometry") == 0) {
            (void)sscanf(argv[index + 1], "%dx%d", &width, &height);
        }
    }
    if (width <= 0 || height <= 0) {
        return 2;
    }
    size = (size_t)width * (size_t)height * 4u;
    square = malloc(size);
    if (square == NULL) {
        return 2;
    }
    for (;;) {
        float reply[20 * 6];
        size_t have = 0u;

        while (have < size) {
            ssize_t got = read(STDIN_FILENO, square + have, size - have);

            if (got <= 0) {
                return 0;
            }
            have += (size_t)got;
        }
        if (mode != NULL && strcmp(mode, "exit") == 0) {
            return 0;
        }
        (void)memset(reply, 0, sizeof(reply));
        {
            const unsigned char *centre =
                square + (((size_t)height / 2u) * (size_t)width +
                          (size_t)width / 2u) * 4u;

            reply[0] = (float)(centre[2] / 3u);   /* class from RED */
            reply[1] = 0.9f;
            reply[2] = 0.25f;   /* y0 */
            reply[3] = 0.25f;   /* x0 */
            reply[4] = 0.75f;   /* y1 */
            reply[5] = 0.75f;   /* x1 */
        }
        if (mode != NULL && strcmp(mode, "garbage") == 0) {
            reply[6] = NAN;
            reply[7] = 0.9f;
            reply[12] = 5.0f;
            reply[13] = 0.9f;
            reply[14] = 0.6f;   /* y0 below y1 ... */
            reply[15] = 0.6f;
            reply[16] = 0.4f;   /* ... y1 above y0: inverted */
            reply[17] = 0.4f;
        }
        if (write(STDOUT_FILENO, reply, sizeof(reply)) !=
            (ssize_t)sizeof(reply)) {
            return 1;
        }
    }
}
