#include "krtsp_exec.h"
#include "krtsp_probe.h"

#include "kilix_rtsp.h"

#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static const char *fake_path(void)
{
    const char *path = getenv("KRTSP_FAKE_FFMPEG");

    return path != NULL ? path : "build/fake-ffmpeg";
}

static long long monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    return (long long)now.tv_sec * 1000 + (long long)now.tv_nsec / 1000000;
}

static void set_mode(const char *mode)
{
    (void)setenv("KILIX_RTSP_FFPROBE", fake_path(), 1);
    (void)setenv("FAKE_FFPROBE_MODE", mode, 1);
}

static int line_count(const char *path)
{
    FILE *file = fopen(path, "r");
    int lines = 0;
    int character;

    if (file == NULL) {
        return -1;
    }
    while ((character = fgetc(file)) != EOF) {
        lines += character == '\n' ? 1 : 0;
    }
    (void)fclose(file);
    return lines;
}

static bool test_capabilities_are_cached(void)
{
    char log_path[] = "/tmp/krtsp-probe-log-XXXXXX";
    int fd = mkstemp(log_path);

    CHECK(fd >= 0);
    CHECK(close(fd) == 0);
    (void)unsetenv("FAKE_FFPROBE_MODE");
    (void)setenv("FAKE_FFMPEG_PROBE_LOG", log_path, 1);
    (void)setenv("FAKE_FFMPEG_LIBAVFORMAT", "58", 1);
    (void)setenv("FAKE_FFMPEG_MKDIR", "1", 1);

    CHECK(krtsp_ffmpeg_needs_legacy_timeout(fake_path()));
    CHECK(krtsp_ffmpeg_needs_legacy_timeout(fake_path()));
    CHECK(krtsp_ffmpeg_supports_segment_mkdir(fake_path()));
    CHECK(krtsp_ffmpeg_supports_segment_mkdir(fake_path()));
    CHECK(line_count(log_path) == 2);

    (void)unsetenv("FAKE_FFMPEG_PROBE_LOG");
    (void)unlink(log_path);
    return true;
}

static bool test_capability_path_is_not_a_shell_command(void)
{
    char directory[] = "/tmp/krtsp-probe-path-XXXXXX";
    char executable[PATH_MAX];
    char marker[PATH_MAX];
    char original[PATH_MAX];
    char resolved[PATH_MAX];
    bool legacy = false;
    bool injected = false;
    bool setup = false;

    CHECK(getcwd(original, sizeof(original)) != NULL);
    CHECK(realpath(fake_path(), resolved) != NULL);
    CHECK(mkdtemp(directory) != NULL);
    CHECK(snprintf(executable, sizeof(executable),
                   "%s/ffmpeg;touch pwned;#", directory) > 0);
    CHECK(snprintf(marker, sizeof(marker), "%s/pwned", directory) > 0);
    CHECK(symlink(resolved, executable) == 0);
    if (chdir(directory) == 0) {
        setup = true;
        (void)setenv("FAKE_FFMPEG_LIBAVFORMAT", "58", 1);
        legacy = krtsp_ffmpeg_needs_legacy_timeout(executable);
        injected = access("pwned", F_OK) == 0;
        (void)chdir(original);
    }
    (void)unlink(marker);
    (void)unlink(executable);
    (void)rmdir(directory);

    CHECK(setup);
    CHECK(legacy);
    CHECK(!injected);
    return true;
}

static bool test_valid_probe(void)
{
    char output[1024];

    set_mode("valid");
    CHECK(krtsp_run_ffprobe("rtsp://example.invalid/live", true, output,
                            sizeof(output), 1000) == 0);
    CHECK(strstr(output, "codec_type=video") != NULL);
    CHECK(strstr(output, "width=1920") != NULL);
    return true;
}

static bool test_hard_deadline(void)
{
    char output[64];
    long long started;
    long long elapsed;

    set_mode("silent");
    started = monotonic_ms();
    CHECK(krtsp_run_ffprobe("rtsp://example.invalid/live", true, output,
                            sizeof(output), 200) == KRTSP_EXEC_TIMEOUT);
    elapsed = monotonic_ms() - started;
    CHECK(elapsed >= 150 && elapsed < 1500);
    return true;
}

static bool test_output_limit_does_not_deadlock(void)
{
    char output[128];
    long long started;
    long long elapsed;

    set_mode("flood");
    started = monotonic_ms();
    CHECK(krtsp_run_ffprobe("rtsp://example.invalid/live", false, output,
                            sizeof(output), 2000) == KRTSP_EXEC_TRUNCATED);
    elapsed = monotonic_ms() - started;
    CHECK(elapsed >= 0 && elapsed < 1500);
    CHECK(output[sizeof(output) - 1u] == '\0');
    return true;
}

static bool test_exit_status_is_preserved(void)
{
    char output[64];

    set_mode("fail");
    CHECK(krtsp_run_ffprobe("rtsp://example.invalid/live", false, output,
                            sizeof(output), 1000) == 7);
    return true;
}

static bool test_arguments_are_literal(void)
{
    static const char url[] =
        "rtsp://user:pass@example.invalid/a;touch should-not-run";
    char output[1024];

    set_mode("validate");
    (void)setenv("FAKE_FFPROBE_EXPECT_URL", url, 1);
    (void)setenv("FAKE_FFPROBE_EXPECT_TCP", "1", 1);
    CHECK(krtsp_run_ffprobe(url, true, output, sizeof(output), 1000) == 0);
    (void)setenv("FAKE_FFPROBE_EXPECT_TCP", "0", 1);
    CHECK(krtsp_run_ffprobe(url, false, output, sizeof(output), 1000) == 0);
    return true;
}

static bool test_invalid_inputs(void)
{
    char output[8];

    set_mode("valid");
    CHECK(krtsp_run_ffprobe(NULL, true, output, sizeof(output), 1000) ==
          KRTSP_EXEC_ERROR);
    CHECK(krtsp_run_ffprobe("", true, output, sizeof(output), 1000) ==
          KRTSP_EXEC_ERROR);
    CHECK(krtsp_run_ffprobe("rtsp://example.invalid", true, NULL, 0u,
                            1000) == KRTSP_EXEC_ERROR);
    return true;
}

typedef bool (*test_function)(void);

typedef struct test_case {
    const char *name;
    test_function function;
} test_case;

int main(void)
{
    static const test_case tests[] = {
        {"capabilities are cached", test_capabilities_are_cached},
        {"capability path is not a shell command",
         test_capability_path_is_not_a_shell_command},
        {"valid probe", test_valid_probe},
        {"hard deadline", test_hard_deadline},
        {"output limit does not deadlock", test_output_limit_does_not_deadlock},
        {"exit status is preserved", test_exit_status_is_preserved},
        {"arguments are literal", test_arguments_are_literal},
        {"invalid inputs", test_invalid_inputs}
    };
    size_t passed = 0u;

    if (access(fake_path(), X_OK) != 0) {
        (void)fprintf(stderr, "fake executable missing: %s\n", fake_path());
        return 1;
    }
    for (size_t index = 0u; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        bool ok = tests[index].function();

        (void)printf("%s %s\n", ok ? "ok" : "not ok", tests[index].name);
        if (!ok) {
            return 1;
        }
        passed++;
    }
    (void)unsetenv("FAKE_FFPROBE_MODE");
    (void)printf("%zu tests passed\n", passed);
    return 0;
}
