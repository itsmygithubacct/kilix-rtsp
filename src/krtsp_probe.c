/* A bounded, shell-free ffprobe invocation for the command-line probe. */

#include "krtsp_probe.h"

#include "krtsp_exec.h"

#include <stdlib.h>
#include <string.h>

int krtsp_run_ffprobe(const char *url, bool force_tcp, char *output,
                      size_t capacity, int timeout_ms)
{
    const char *binary = getenv("KILIX_RTSP_FFPROBE");
    char *argv[24];
    size_t at = 0u;

    if (url == NULL || url[0] == '\0') {
        return KRTSP_EXEC_ERROR;
    }
    if (binary == NULL || binary[0] == '\0') {
        binary = "ffprobe";
    }
    argv[at++] = (char *)binary;
    argv[at++] = (char *)"-hide_banner";
    argv[at++] = (char *)"-loglevel";
    argv[at++] = (char *)"error";
    if (force_tcp) {
        argv[at++] = (char *)"-rtsp_transport";
        argv[at++] = (char *)"tcp";
    }
    /* A socket timeout is a socket option: the file protocol has none, and
     * ffprobe exits rather than ignoring an option it does not know.  So a
     * local recording could not be probed at all. */
    if (strncmp(url, "rtsp://", 7) == 0 || strncmp(url, "rtsps://", 8) == 0 ||
        strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
        argv[at++] = (char *)"-timeout";
        argv[at++] = (char *)"5000000";
    }
    /* Low-bitrate substreams need enough input to determine geometry. */
    argv[at++] = (char *)"-analyzeduration";
    argv[at++] = (char *)"5000000";
    argv[at++] = (char *)"-probesize";
    argv[at++] = (char *)"5000000";
    argv[at++] = (char *)"-show_entries";
    argv[at++] = (char *)
        "stream=index,codec_type,codec_name,profile,width,height,"
        "pix_fmt,r_frame_rate,avg_frame_rate,has_b_frames,sample_rate,"
        "channels";
    argv[at++] = (char *)"-of";
    argv[at++] = (char *)"default=noprint_wrappers=0";
    argv[at++] = (char *)url;
    argv[at] = NULL;

    return krtsp_exec_capture(binary, argv, output, capacity, timeout_ms);
}
