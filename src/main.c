/*
 * The kilix-rtsp command.
 *
 *   kilix-rtsp list                 cameras and groups from the config
 *   kilix-rtsp probe <target>       stream properties
 *
 * `target` is a camera name from the configuration or a bare RTSP URL, so
 * the command is useful before any configuration exists.
 */

#include "kilix_rtsp.h"
#include "krtsp_view.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void usage(FILE *stream)
{
    (void)fprintf(stream,
        "usage: kilix-rtsp <command> [options]\n"
        "\n"
        "  list                     cameras and groups from the config\n"
        "  probe <name|url>         stream properties for one camera\n"
        "  view  <name|url>         one camera filling the terminal\n"
        "\n"
        "options:\n"
        "  --tier main|sub          stream tier (default: sub for probe,\n"
        "                           main for view)\n"
        "  --fps <n>                cap the delivered frame rate (view)\n"
        "  --tab                    open the view in a new kilix tab\n"
        "  --config <path>          config file (default\n"
        "                           <state>/config/cameras.conf)\n"
        "\n"
        "Configuration and data live under ~/.local/gpu_terminal/kilix-rtsp,\n"
        "overridable with KILIX_RTSP_HOME.  The ffmpeg binary can be\n"
        "overridden with KILIX_RTSP_FFMPEG.\n");
}

/* Load the config, or explain why not.  A missing file is not fatal for a
 * command that was given a URL. */
static krtsp_config *load_config_or_warn(const char *path, bool required)
{
    krtsp_config *config = NULL;
    char error[512];

    if (!krtsp_config_load(&config, path, error, sizeof(error))) {
        if (required) {
            (void)fprintf(stderr, "kilix-rtsp: %s\n", error);
        }
        return NULL;
    }
    return config;
}

static int command_list(const char *config_path)
{
    krtsp_config *config = load_config_or_warn(config_path, true);
    size_t cameras;
    size_t groups;

    if (config == NULL) {
        return 1;
    }
    cameras = krtsp_config_camera_count(config);
    groups = krtsp_config_group_count(config);

    if (cameras == 0u) {
        (void)printf("no cameras configured\n");
    }
    for (size_t index = 0u; index < cameras; ++index) {
        const krtsp_camera *camera = krtsp_config_camera_at(config, index);
        char safe_main[KRTSP_URL_MAX];
        char safe_sub[KRTSP_URL_MAX];

        /* Never print a URL unredacted: this output goes into terminals,
         * scrollback, and pasted bug reports. */
        if (!krtsp_url_redact(camera->url_main[0] != '\0' ? camera->url_main
                                                          : "(none)",
                              safe_main, sizeof(safe_main))) {
            (void)snprintf(safe_main, sizeof(safe_main), "(unprintable)");
        }
        if (!krtsp_url_redact(camera->url_sub[0] != '\0' ? camera->url_sub
                                                         : "(none)",
                              safe_sub, sizeof(safe_sub))) {
            (void)snprintf(safe_sub, sizeof(safe_sub), "(unprintable)");
        }
        (void)printf("camera %s\n", camera->name);
        (void)printf("  main %s\n", safe_main);
        (void)printf("  sub  %s\n", safe_sub);
    }
    for (size_t index = 0u; index < groups; ++index) {
        const krtsp_group *group = krtsp_config_group_at(config, index);

        (void)printf("group %s:", group->name);
        for (size_t member = 0u; member < group->member_count; ++member) {
            (void)printf(" %s", group->members[member]);
        }
        (void)printf("\n");
    }
    krtsp_config_free(config);
    return 0;
}

/*
 * Run ffprobe and print what it says.
 *
 * ffprobe's own -timeout governs the socket, not the process, so it is
 * backed by a hard kill here - a camera that accepts a connection and
 * then says nothing outlives the internal timeout.  That part follows
 * Frigate, which has probed a lot of cameras.
 *
 * Where this diverges: Frigate tries the default transport first and
 * retries over TCP on failure.  On this fleet UDP does not fail, it
 * succeeds emptily - exit 0, width=0, pix_fmt=unknown - so a
 * retry-on-failure never fires.  TCP goes first instead.
 */
static int run_ffprobe(const char *url, bool force_tcp, char *output,
                       size_t capacity, int timeout_seconds)
{
    const char *binary = getenv("KILIX_RTSP_FFPROBE");
    int fds[2];
    pid_t pid;
    size_t used = 0u;
    int status = -1;
    int waited_ms = 0;

    if (binary == NULL || binary[0] == '\0') {
        binary = "ffprobe";
    }
    output[0] = '\0';
    if (pipe(fds) != 0) {
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        char *argv[24];
        size_t at = 0u;

        (void)close(fds[0]);
        (void)dup2(fds[1], STDOUT_FILENO);
        (void)close(fds[1]);
        {
            int null_fd = open("/dev/null", O_WRONLY);

            if (null_fd >= 0) {
                (void)dup2(null_fd, STDERR_FILENO);
                (void)close(null_fd);
            }
        }
        argv[at++] = (char *)binary;
        argv[at++] = (char *)"-hide_banner";
        argv[at++] = (char *)"-loglevel";
        argv[at++] = (char *)"error";
        if (force_tcp) {
            argv[at++] = (char *)"-rtsp_transport";
            argv[at++] = (char *)"tcp";
        }
        argv[at++] = (char *)"-timeout";
        argv[at++] = (char *)"5000000";
        /*
         * Give ffprobe enough stream to actually determine the format.
         * Without this a low-bitrate substream returns exit 0 with
         * width=0, pix_fmt=unknown and a nonsense frame rate - a probe
         * that looks successful and says nothing.  A camera at 8 fps
         * takes a while to produce a second of video.
         */
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
        execvp(binary, argv);
        _exit(127);
    }

    (void)close(fds[1]);
    for (;;) {
        ssize_t count = read(fds[0], output + used, capacity - used - 1u);

        if (count > 0) {
            used += (size_t)count;
            output[used] = '\0';
            if (used + 1u >= capacity) {
                break;
            }
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    (void)close(fds[0]);

    /* Hard bound on the child.  ffprobe's -timeout governs the socket,
     * not the process, and a camera that accepts a connection then says
     * nothing can outlive it. */
    while (waited_ms < timeout_seconds * 1000) {
        if (waitpid(pid, &status, WNOHANG) == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        usleep(50000);
        waited_ms += 50;
    }
    (void)kill(pid, SIGKILL);
    (void)waitpid(pid, NULL, 0);
    return -2;   /* timed out */
}

/*
 * Resolve a camera name or a bare URL to a URL and a label.
 *
 * The label is what appears on screen and in messages; it is never a URL,
 * because a URL is a password.
 */
static bool resolve_target(
    const char *target, krtsp_tier tier, const char *config_path,
    krtsp_config **config_out, const char **url_out, const char **label_out)
{
    *config_out = NULL;
    *url_out = NULL;
    *label_out = NULL;

    if (target == NULL) {
        (void)fprintf(stderr, "kilix-rtsp: expected a camera name or URL\n");
        return false;
    }
    if (strstr(target, "://") != NULL) {
        *url_out = target;
        *label_out = "stream";
        return true;
    }
    {
        krtsp_config *config = load_config_or_warn(config_path, true);
        const krtsp_camera *camera;

        if (config == NULL) {
            return false;
        }
        camera = krtsp_config_find(config, target);
        if (camera == NULL) {
            (void)fprintf(stderr, "kilix-rtsp: no camera named '%s'\n", target);
            krtsp_config_free(config);
            return false;
        }
        *url_out = krtsp_camera_url(camera, tier);
        if (*url_out == NULL) {
            (void)fprintf(stderr, "kilix-rtsp: '%s' has no usable url\n",
                          target);
            krtsp_config_free(config);
            return false;
        }
        *config_out = config;
        *label_out = camera->name;
        return true;
    }
}

/*
 * Re-launch this command in a new kilix tab.
 *
 * The mechanism is the one the kilix desktop already uses to start apps:
 * kitty remote control, targeted at the socket named by KITTY_LISTEN_ON.
 * Nothing here is kilix-specific beyond that variable - it works in any
 * kitty with remote control enabled.
 *
 * Returns the child's exit status, or -1 when a tab is not available, in
 * which case the caller runs inline instead.  Falling back is the right
 * behaviour: a viewer that refuses to start because it could not open a
 * tab would be worse than one that simply uses the terminal it has.
 */
static int relaunch_in_tab(char **argv, const char *label)
{
    static const char *const forwarded[] = {
        "KILIX_RTSP_HOME", "KILIX_RTSP_FFMPEG", "KILIX_RTSP_FFPROBE"
    };
    static char env_pairs[3][640];
    const char *listen_on = getenv("KITTY_LISTEN_ON");
    char self[512];
    char *child[KRTSP_ARGV_MAX];
    ssize_t length;
    size_t at = 0u;
    pid_t pid;
    int status = 0;

    if (listen_on == NULL || listen_on[0] == '\0') {
        (void)fprintf(stderr,
            "kilix-rtsp: --tab needs kitty remote control (KITTY_LISTEN_ON is\n"
            "            not set); running in this terminal instead.\n");
        return -1;
    }
    length = readlink("/proc/self/exe", self, sizeof(self) - 1u);
    if (length <= 0) {
        return -1;
    }
    self[length] = '\0';

    child[at++] = (char *)"kitten";
    child[at++] = (char *)"@";
    child[at++] = (char *)"--to";
    child[at++] = (char *)listen_on;
    child[at++] = (char *)"launch";
    child[at++] = (char *)"--type=tab";
    child[at++] = (char *)"--tab-title";
    child[at++] = (char *)label;
    /*
     * A launched tab inherits the terminal's environment, not this
     * process's, so anything set in the shell that ran this command has
     * to be forwarded explicitly.  Without this the new tab cannot find
     * the configuration, fails immediately, and the tab closes again
     * before the error can be read - which looks like nothing happened.
     */
    for (size_t index = 0u; index < sizeof(forwarded) / sizeof(forwarded[0]);
         ++index) {
        const char *value = getenv(forwarded[index]);

        if (value == NULL || value[0] == '\0' ||
            at + 6u >= KRTSP_ARGV_MAX) {
            continue;
        }
        if (snprintf(env_pairs[index], sizeof(env_pairs[index]), "%s=%s",
                     forwarded[index], value) < 0) {
            continue;
        }
        child[at++] = (char *)"--env";
        child[at++] = env_pairs[index];
    }
    child[at++] = (char *)"--";
    child[at++] = self;
    /* Copy the original arguments, dropping --tab so the child does not
     * try to open a tab of its own. */
    for (int index = 1; argv[index] != NULL && at + 2u < KRTSP_ARGV_MAX;
         ++index) {
        if (strcmp(argv[index], "--tab") == 0) {
            continue;
        }
        child[at++] = argv[index];
    }
    child[at] = NULL;

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp("kitten", child);
        /* Older layouts expose remote control through the terminal
         * binary rather than a separate kitten. */
        child[0] = (char *)"kilix";
        execvp("kilix", child);
        child[0] = (char *)"kitty";
        execvp("kitty", child);
        _exit(127);
    }
    (void)waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }
    (void)fprintf(stderr,
        "kilix-rtsp: could not open a tab; running in this terminal.\n");
    return -1;
}

static int command_view(const char *target, krtsp_tier tier,
                        const char *config_path, int fps_cap)
{
    krtsp_config *config = NULL;
    const char *url = NULL;
    const char *label = NULL;
    int result;

    if (!resolve_target(target, tier, config_path, &config, &url, &label)) {
        return 2;
    }
    result = krtsp_view_run(url, label, fps_cap);
    krtsp_config_free(config);
    return result;
}

static int command_probe(const char *target, krtsp_tier tier,
                         const char *config_path)
{
    krtsp_config *config;
    const char *url = NULL;
    char safe[KRTSP_URL_MAX];
    char escaped[KRTSP_ARGV_STORAGE_MAX];
    static char output[16384];
    int result;

    if (target == NULL) {
        (void)fprintf(stderr, "kilix-rtsp: probe needs a camera name or URL\n");
        return 2;
    }

    if (strstr(target, "://") != NULL) {
        url = target;
        config = NULL;
    } else {
        config = load_config_or_warn(config_path, true);
        if (config == NULL) {
            return 1;
        }
        const krtsp_camera *camera = krtsp_config_find(config, target);

        if (camera == NULL) {
            (void)fprintf(stderr, "kilix-rtsp: no camera named '%s'\n", target);
            krtsp_config_free(config);
            return 1;
        }
        url = krtsp_camera_url(camera, tier);
        if (url == NULL) {
            (void)fprintf(stderr, "kilix-rtsp: '%s' has no usable url\n",
                          target);
            krtsp_config_free(config);
            return 1;
        }
    }

    if (!krtsp_url_escape_password(url, escaped, sizeof(escaped))) {
        (void)fprintf(stderr, "kilix-rtsp: url too long\n");
        krtsp_config_free(config);
        return 1;
    }
    if (!krtsp_url_redact(escaped, safe, sizeof(safe))) {
        (void)snprintf(safe, sizeof(safe), "(unprintable)");
    }
    (void)fprintf(stderr, "probing %s (%s)\n", safe,
                  tier == KRTSP_TIER_MAIN ? "main" : "sub");

    /*
     * TCP first, not as a fallback.  The default transport is UDP, and on
     * this camera fleet UDP does not fail cleanly - it returns exit 0 with
     * width=0 and pix_fmt=unknown, which is worse than an error because
     * nothing downstream can tell it apart from a successful probe.  TCP
     * is also what the streaming path uses, so probing over it describes
     * the transport that will actually be used.
     */
    result = run_ffprobe(escaped, true, output, sizeof(output), 20);
    if (result != 0) {
        (void)fprintf(stderr,
                      "  (retrying with the default transport)\n");
        result = run_ffprobe(escaped, false, output, sizeof(output), 15);
    }
    /* An exit status of 0 is not proof the probe learned anything. */
    if (result == 0 && strstr(output, "codec_type=video") != NULL &&
        strstr(output, "width=0") != NULL) {
        (void)fprintf(stderr,
            "kilix-rtsp: the camera answered but did not describe its video\n"
            "            stream (width=0).  It may need longer than the\n"
            "            probe allows, or it may only speak a transport this\n"
            "            probe did not use.\n");
        krtsp_config_free(config);
        return 1;
    }

    if (result == -2) {
        (void)fprintf(stderr,
            "kilix-rtsp: probe timed out.  A camera can accept a connection\n"
            "            and then send nothing; that looks identical to a\n"
            "            slow one until the timer fires.\n");
        krtsp_config_free(config);
        return 1;
    }
    if (result != 0 || output[0] == '\0') {
        (void)fprintf(stderr, "kilix-rtsp: probe failed (ffprobe exit %d)\n",
                      result);
        krtsp_config_free(config);
        return 1;
    }
    (void)fputs(output, stdout);
    krtsp_config_free(config);
    return 0;
}

int main(int argc, char **argv)
{
    const char *command;
    const char *target = NULL;
    const char *config_path = NULL;
    krtsp_tier tier = KRTSP_TIER_SUB;
    bool tier_given = false;
    bool want_tab = false;
    int fps_cap = 0;

    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    command = argv[1];
    if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0 ||
        strcmp(command, "help") == 0) {
        usage(stdout);
        return 0;
    }

    for (int index = 2; index < argc; ++index) {
        if (strcmp(argv[index], "--tier") == 0 && index + 1 < argc) {
            const char *value = argv[++index];

            tier_given = true;
            if (strcmp(value, "main") == 0) {
                tier = KRTSP_TIER_MAIN;
            } else if (strcmp(value, "sub") == 0) {
                tier = KRTSP_TIER_SUB;
            } else {
                (void)fprintf(stderr,
                              "kilix-rtsp: --tier takes 'main' or 'sub'\n");
                return 2;
            }
        } else if (strcmp(argv[index], "--config") == 0 && index + 1 < argc) {
            config_path = argv[++index];
        } else if (strcmp(argv[index], "--tab") == 0) {
            want_tab = true;
        } else if (strcmp(argv[index], "--fps") == 0 && index + 1 < argc) {
            fps_cap = atoi(argv[++index]);
            if (fps_cap < 0) {
                (void)fprintf(stderr, "kilix-rtsp: --fps cannot be negative\n");
                return 2;
            }
        } else if (argv[index][0] == '-') {
            (void)fprintf(stderr, "kilix-rtsp: unknown option %s\n",
                          argv[index]);
            return 2;
        } else if (target == NULL) {
            target = argv[index];
        } else {
            (void)fprintf(stderr, "kilix-rtsp: unexpected argument %s\n",
                          argv[index]);
            return 2;
        }
    }

    if (strcmp(command, "list") == 0) {
        return command_list(config_path);
    }
    if (strcmp(command, "probe") == 0) {
        return command_probe(target, tier, config_path);
    }
    if (strcmp(command, "view") == 0) {
        /* A view fills the terminal, so it wants the main stream unless
         * told otherwise; presenting an upscaled substream throws away
         * resolution the camera is already producing. */
        if (!tier_given) {
            tier = KRTSP_TIER_MAIN;
        }
        if (want_tab) {
            krtsp_config *probe_config = NULL;
            const char *probe_url = NULL;
            const char *probe_label = NULL;

            /* Resolve here, before opening the tab.  A bad camera name or
             * an unreadable config would otherwise fail inside the new
             * tab, which closes immediately and takes the message with
             * it - indistinguishable from nothing having happened. */
            if (!resolve_target(target, tier, config_path, &probe_config,
                                &probe_url, &probe_label)) {
                return 2;
            }
            {
                char tab_label[KRTSP_NAME_MAX];

                (void)snprintf(tab_label, sizeof(tab_label), "%s",
                               probe_label);
                krtsp_config_free(probe_config);
                if (relaunch_in_tab(argv, tab_label) == 0) {
                    return 0;
                }
            }
        }
        return command_view(target, tier, config_path, fps_cap);
    }
    (void)fprintf(stderr, "kilix-rtsp: unknown command '%s'\n", command);
    usage(stderr);
    return 2;
}
