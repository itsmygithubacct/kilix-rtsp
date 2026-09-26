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
#include "krtsp_exec.h"
#include "krtsp_probe.h"
#include "krtsp_view.h"

#include <errno.h>
#include <limits.h>
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
        "  mosaic <group|name...>   several cameras in a grid\n"
        "\n"
        "options:\n"
        "  --tier main|sub          stream tier (default: sub for probe,\n"
        "                           main for view)\n"
        "  --fps <n>                cap the delivered frame rate (view)\n"
        "  --tab                    open the view in a new kilix tab\n"
        "  --buffer <size|time|off> history kept behind the live view (view):\n"
        "                           auto (default), off, 2G, 500M, 90s, 10min\n"
        "  --detect                 start the view with object detection on\n"
        "  --config <path>          config file (default\n"
        "                           <state>/config/cameras.conf)\n"
        "\n"
        "Configuration and data live under ~/.local/gpu_terminal/kilix-rtsp,\n"
        "overridable with KILIX_RTSP_HOME.  The ffmpeg binary can be\n"
        "overridden with KILIX_RTSP_FFMPEG.\n"
        "\n"
        "In a view: Left/Right step 10s through the history, Shift or\n"
        "PgUp/PgDn 60s, Home the oldest, End back to live, Space pause,\n"
        "d object detection, ? help, q quit.  The default buffer is sized\n"
        "from free disk; set KILIX_RTSP_BUFFER or `buffer = 2G` in\n"
        "<state>/config/settings.conf to change it.\n");
}

static bool parse_nonnegative_int(const char *text, int *out)
{
    char *end;
    long value;

    if (text == NULL || text[0] == '\0' || out == NULL) {
        return false;
    }
    for (const unsigned char *scan = (const unsigned char *)text;
         *scan != '\0'; ++scan) {
        if (*scan < '0' || *scan > '9') {
            return false;
        }
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < 0 || value > INT_MAX) {
        return false;
    }
    *out = (int)value;
    return true;
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
    if (length <= 0 || (size_t)length >= sizeof(self) - 1u) {
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
                     forwarded[index], value) < 0 ||
            strlen(forwarded[index]) + strlen(value) + 2u >
                sizeof(env_pairs[index])) {
            continue;
        }
        child[at++] = (char *)"--env";
        child[at++] = env_pairs[index];
    }
    child[at++] = (char *)"--";
    child[at++] = self;
    /* Copy the original arguments, dropping --tab so the child does not
     * try to open a tab of its own. */
    for (int index = 1; argv[index] != NULL; ++index) {
        if (strcmp(argv[index], "--tab") == 0) {
            continue;
        }
        if (at + 1u >= KRTSP_ARGV_MAX) {
            (void)fprintf(stderr,
                          "kilix-rtsp: too many arguments to open a tab\n");
            return -1;
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
    {
        pid_t waited;

        do {
            waited = waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return 0;
        }
    }
    (void)fprintf(stderr,
        "kilix-rtsp: could not open a tab; running in this terminal.\n");
    return -1;
}

/*
 * Resolve a group name, or a list of camera names, into urls and labels.
 * A mosaic tile is far smaller than a main stream, so tiles take the sub
 * stream: the extra resolution would be decoded only to be discarded.
 */
static int command_mosaic(
    char **names, size_t name_count, const char *config_path, int fps_cap)
{
    krtsp_config *config = load_config_or_warn(config_path, true);
    const char *urls[16];
    const char *labels[16];
    size_t count = 0u;
    int result;

    if (config == NULL) {
        return 1;
    }
    if (name_count == 0u) {
        /* No argument: every configured camera, which is the common case
         * for a wall display. */
        count = krtsp_config_camera_count(config);
        if (count > 16u) {
            (void)fprintf(stderr,
                          "kilix-rtsp: mosaic supports at most 16 cameras\n");
            krtsp_config_free(config);
            return 2;
        }
        for (size_t index = 0u; index < count; ++index) {
            const krtsp_camera *camera = krtsp_config_camera_at(config, index);

            urls[index] = krtsp_camera_url(camera, KRTSP_TIER_SUB);
            labels[index] = camera->name;
        }
    } else if (name_count == 1u &&
               krtsp_config_find_group(config, names[0]) != NULL) {
        const krtsp_group *group = krtsp_config_find_group(config, names[0]);

        if (group->member_count > 16u) {
            (void)fprintf(stderr,
                          "kilix-rtsp: group '%s' has more than 16 cameras\n",
                          group->name);
            krtsp_config_free(config);
            return 2;
        }
        count = group->member_count;
        for (size_t index = 0u; index < count; ++index) {
            const krtsp_camera *camera =
                krtsp_config_find(config, group->members[index]);

            urls[index] = krtsp_camera_url(camera, KRTSP_TIER_SUB);
            labels[index] = camera->name;
        }
    } else {
        for (size_t index = 0u; index < name_count && index < 16u; ++index) {
            const krtsp_camera *camera =
                krtsp_config_find(config, names[index]);

            if (camera == NULL) {
                (void)fprintf(stderr,
                              "kilix-rtsp: no camera or group named '%s'\n",
                              names[index]);
                krtsp_config_free(config);
                return 1;
            }
            urls[index] = krtsp_camera_url(camera, KRTSP_TIER_SUB);
            labels[index] = camera->name;
            count++;
        }
    }
    if (count == 0u) {
        (void)fprintf(stderr, "kilix-rtsp: no cameras to show\n");
        krtsp_config_free(config);
        return 1;
    }
    result = krtsp_mosaic_run(urls, labels, count, fps_cap);
    krtsp_config_free(config);
    return result;
}

/*
 * The default history size: the settings file, then the environment, then
 * what --buffer said.  A value that does not parse is reported and ignored
 * rather than trusted: a buffer sized by a guess is a disk filled by one.
 */
static void buffer_from_text(const char *origin, const char *text,
                             krtsp_buffer_spec *spec)
{
    krtsp_buffer_spec parsed;

    if (krtsp_buffer_parse(text, &parsed)) {
        *spec = parsed;
        return;
    }
    (void)fprintf(stderr,
                  "kilix-rtsp: %s: cannot read buffer size '%s' (try 2G, "
                  "500M, 90s, 10min, auto or off)\n", origin, text);
}

static void buffer_from_settings_file(krtsp_buffer_spec *spec)
{
    char directory[PATH_MAX];
    char path[PATH_MAX + 16];
    char line[256];
    FILE *file;

    if (!krtsp_paths_dir("config", directory, sizeof(directory)) ||
        snprintf(path, sizeof(path), "%s/settings.conf", directory) >=
            (int)sizeof(path)) {
        return;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *equals = strchr(line, '=');
        char *key = line;
        char *value;
        char *end;

        if (equals == NULL || line[0] == '#') {
            continue;
        }
        *equals = '\0';
        value = equals + 1;
        while (*key == ' ' || *key == '\t') {
            ++key;
        }
        end = key + strlen(key);
        while (end > key && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        while (*value == ' ' || *value == '\t') {
            ++value;
        }
        end = value + strlen(value);
        while (end > value && (end[-1] == ' ' || end[-1] == '\t' ||
                               end[-1] == '\n' || end[-1] == '\r')) {
            *--end = '\0';
        }
        if (strcmp(key, "buffer") == 0) {
            buffer_from_text("settings.conf", value, spec);
        }
    }
    (void)fclose(file);
}

static int command_view(const char *target, krtsp_tier tier,
                        const char *config_path, int fps_cap,
                        const char *buffer_text, bool detect)
{
    krtsp_config *config = NULL;
    const char *url = NULL;
    const char *label = NULL;
    krtsp_view_options options;
    const char *environment = getenv("KILIX_RTSP_BUFFER");
    int result;

    if (!resolve_target(target, tier, config_path, &config, &url, &label)) {
        return 2;
    }
    (void)memset(&options, 0, sizeof(options));
    options.fps_cap = fps_cap;
    options.detect = detect;
    buffer_from_settings_file(&options.buffer);
    if (environment != NULL && environment[0] != '\0') {
        buffer_from_text("KILIX_RTSP_BUFFER", environment, &options.buffer);
    }
    if (buffer_text != NULL) {
        buffer_from_text("--buffer", buffer_text, &options.buffer);
    }
    result = krtsp_view_run(url, label, &options);
    krtsp_config_free(config);
    return result;
}

static bool probe_output_has_video(const char *output)
{
    return output != NULL && strstr(output, "codec_type=video") != NULL;
}

static bool probe_output_is_useful(const char *output)
{
    return probe_output_has_video(output) &&
           strstr(output, "width=0") == NULL &&
           strstr(output, "pix_fmt=unknown") == NULL;
}

static int command_probe(const char *target, krtsp_tier tier,
                         const char *config_path)
{
    krtsp_config *config = NULL;
    const char *url = NULL;
    const char *label = NULL;
    char safe[KRTSP_URL_MAX];
    char escaped[KRTSP_ARGV_STORAGE_MAX];
    static char output[16384];
    int result;

    if (!resolve_target(target, tier, config_path, &config, &url, &label)) {
        return 1;
    }
    (void)label;

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
    result = krtsp_run_ffprobe(escaped, true, output, sizeof(output), 20000);
    if (result != 0 || !probe_output_is_useful(output)) {
        (void)fprintf(stderr,
                      "  (retrying with the default transport)\n");
        result = krtsp_run_ffprobe(escaped, false, output, sizeof(output),
                                   15000);
    }
    /* An exit status of 0 is not proof the probe learned anything. */
    if (result == 0 && probe_output_has_video(output) &&
        !probe_output_is_useful(output)) {
        (void)fprintf(stderr,
            "kilix-rtsp: the camera answered but did not describe its video\n"
            "            stream (zero width or unknown pixel format).  It\n"
            "            may need longer than the\n"
            "            probe allows, or it may only speak a transport this\n"
            "            probe did not use.\n");
        krtsp_config_free(config);
        return 1;
    }

    if (result == 0 && output[0] != '\0' &&
        !probe_output_has_video(output)) {
        (void)fprintf(stderr,
                      "kilix-rtsp: probe found no video stream\n");
        krtsp_config_free(config);
        return 1;
    }

    if (result == KRTSP_EXEC_TIMEOUT) {
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
    bool fps_given = false;
    int fps_cap = 0;
    const char *buffer_text = NULL;
    bool detect = false;
    char *positional[16];
    int positional_count = 0;

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
    if (strcmp(command, "list") != 0 && strcmp(command, "probe") != 0 &&
        strcmp(command, "view") != 0 && strcmp(command, "mosaic") != 0) {
        (void)fprintf(stderr, "kilix-rtsp: unknown command '%s'\n", command);
        usage(stderr);
        return 2;
    }

    for (int index = 2; index < argc; ++index) {
        if (strcmp(argv[index], "--tier") == 0) {
            if (index + 1 >= argc) {
                (void)fprintf(stderr,
                              "kilix-rtsp: --tier needs a value\n");
                return 2;
            }
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
        } else if (strcmp(argv[index], "--config") == 0) {
            if (index + 1 >= argc) {
                (void)fprintf(stderr,
                              "kilix-rtsp: --config needs a path\n");
                return 2;
            }
            config_path = argv[++index];
        } else if (strcmp(argv[index], "--tab") == 0) {
            want_tab = true;
        } else if (strcmp(argv[index], "--fps") == 0) {
            if (index + 1 >= argc ||
                !parse_nonnegative_int(argv[index + 1], &fps_cap)) {
                (void)fprintf(stderr,
                              "kilix-rtsp: --fps needs a non-negative integer\n");
                return 2;
            }
            index++;
            fps_given = true;
        } else if (strcmp(argv[index], "--buffer") == 0) {
            if (index + 1 >= argc) {
                (void)fprintf(stderr,
                              "kilix-rtsp: --buffer needs a size, a time, "
                              "auto or off\n");
                return 2;
            }
            buffer_text = argv[++index];
        } else if (strcmp(argv[index], "--detect") == 0) {
            detect = true;
        } else if (argv[index][0] == '-') {
            (void)fprintf(stderr, "kilix-rtsp: unknown option %s\n",
                          argv[index]);
            return 2;
        } else {
            if (target == NULL) {
                target = argv[index];
            }
            if (positional_count >=
                (int)(sizeof(positional) / sizeof(positional[0]))) {
                (void)fprintf(stderr,
                              "kilix-rtsp: at most 16 camera names are allowed\n");
                return 2;
            }
            positional[positional_count++] = argv[index];
        }
    }

    if (strcmp(command, "view") != 0 && (buffer_text != NULL || detect)) {
        (void)fprintf(stderr,
                      "kilix-rtsp: --buffer and --detect apply to view only\n");
        return 2;
    }
    if (buffer_text != NULL) {
        /* Refuse a malformed size before opening a tab or a terminal, where
         * the message would vanish with the window. */
        krtsp_buffer_spec check;

        if (!krtsp_buffer_parse(buffer_text, &check)) {
            (void)fprintf(stderr,
                          "kilix-rtsp: --buffer '%s' is not a size or a time "
                          "(try 2G, 500M, 90s, 10min, auto or off)\n",
                          buffer_text);
            return 2;
        }
    }

    if (strcmp(command, "list") == 0) {
        if (positional_count != 0 || tier_given || fps_given || want_tab) {
            (void)fprintf(stderr,
                          "kilix-rtsp: list takes only --config\n");
            return 2;
        }
        return command_list(config_path);
    }
    if (strcmp(command, "probe") == 0) {
        if (positional_count != 1 || fps_given || want_tab) {
            (void)fprintf(stderr,
                          "kilix-rtsp: probe needs one target; --fps and "
                          "--tab do not apply\n");
            return 2;
        }
        return command_probe(target, tier, config_path);
    }
    if (strcmp(command, "view") == 0) {
        if (positional_count != 1) {
            (void)fprintf(stderr,
                          "kilix-rtsp: view needs exactly one target\n");
            return 2;
        }
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
        return command_view(target, tier, config_path, fps_cap, buffer_text,
                            detect);
    }
    if (strcmp(command, "mosaic") == 0) {
        if (tier_given || want_tab) {
            (void)fprintf(stderr,
                          "kilix-rtsp: mosaic uses substreams and does not "
                          "support --tier or --tab\n");
            return 2;
        }
        return command_mosaic(positional, (size_t)positional_count,
                              config_path, fps_cap);
    }
    return 2;
}
