#include "kilix_rtsp.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

static char scratch_dir[512];

static const char *temp_path(const char *name)
{
    static char path[640];

    (void)snprintf(path, sizeof(path), "%s/%s", scratch_dir, name);
    return path;
}

/* Write a config file at mode 0600, which is what the loader requires. */
static bool write_config(const char *name, const char *contents, mode_t mode)
{
    const char *path = temp_path(name);
    FILE *handle = fopen(path, "w");

    if (handle == NULL) {
        return false;
    }
    (void)fputs(contents, handle);
    (void)fclose(handle);
    return chmod(path, mode) == 0;
}

static bool write_binary_config(const char *name, const void *contents,
                                size_t size, mode_t mode)
{
    const char *path = temp_path(name);
    FILE *handle = fopen(path, "wb");

    if (handle == NULL) {
        return false;
    }
    if (fwrite(contents, 1u, size, handle) != size || fclose(handle) != 0) {
        return false;
    }
    return chmod(path, mode) == 0;
}

static bool load_fails(const char *name, const char *contents,
                       const char *error_fragment)
{
    krtsp_config *config = NULL;
    char error[256];

    if (!write_config(name, contents, 0600) ||
        krtsp_config_load(&config, temp_path(name), error, sizeof(error))) {
        krtsp_config_free(config);
        return false;
    }
    return config == NULL &&
           (error_fragment == NULL || strstr(error, error_fragment) != NULL);
}

static bool
test_parses_cameras_and_groups(void)
{
    krtsp_config *config = NULL;
    char error[256];
    const krtsp_camera *camera;
    const krtsp_group *group;

    CHECK(write_config("good.conf",
        "# a comment\n"
        "\n"
        "[camera \"front\"]\n"
        "main = rtsp://u:p@203.0.113.10:554/stream1\n"
        "sub  = rtsp://u:p@203.0.113.10:554/stream2\n"
        "\n"
        "[camera \"garage\"]\n"
        "main = rtsp://u:p@203.0.113.12:10554/tcp/av0_0\n"
        "sub  = rtsp://u:p@203.0.113.12:10554/tcp/av0_1\n"
        "\n"
        "[group \"all\"]\n"
        "cameras = front, garage\n", 0600));

    CHECK(krtsp_config_load(&config, temp_path("good.conf"), error,
                            sizeof(error)));
    CHECK(error[0] == '\0');
    CHECK(krtsp_config_camera_count(config) == 2u);

    camera = krtsp_config_find(config, "front");
    CHECK(camera != NULL);
    CHECK(strcmp(krtsp_camera_url(camera, KRTSP_TIER_MAIN),
                 "rtsp://u:p@203.0.113.10:554/stream1") == 0);
    CHECK(strcmp(krtsp_camera_url(camera, KRTSP_TIER_SUB),
                 "rtsp://u:p@203.0.113.10:554/stream2") == 0);

    /* A non-standard port and a third path convention must survive
     * verbatim: nothing here derives one tier's URL from another's. */
    camera = krtsp_config_find(config, "garage");
    CHECK(camera != NULL);
    CHECK(strstr(krtsp_camera_url(camera, KRTSP_TIER_SUB), ":10554/") != NULL);

    group = krtsp_config_find_group(config, "all");
    CHECK(group != NULL);
    CHECK(group->member_count == 2u);
    CHECK(strcmp(group->members[0], "front") == 0);
    CHECK(strcmp(group->members[1], "garage") == 0);

    CHECK(krtsp_config_find(config, "nope") == NULL);
    CHECK(krtsp_config_find_group(config, "nope") == NULL);

    krtsp_config_free(config);
    return true;
}

static bool
test_tier_fallback(void)
{
    krtsp_config *config = NULL;
    char error[256];
    const krtsp_camera *camera;

    /* A camera defined with only a substream still has to work in a
     * full-terminal view; refusing would be pedantry. */
    CHECK(write_config("sub_only.conf",
        "[camera \"cheap\"]\n"
        "sub = rtsp://u:p@203.0.113.20/2\n", 0600));
    CHECK(krtsp_config_load(&config, temp_path("sub_only.conf"), error,
                            sizeof(error)));
    camera = krtsp_config_find(config, "cheap");
    CHECK(camera != NULL);
    CHECK(strcmp(krtsp_camera_url(camera, KRTSP_TIER_SUB),
                 "rtsp://u:p@203.0.113.20/2") == 0);
    CHECK(strcmp(krtsp_camera_url(camera, KRTSP_TIER_MAIN),
                 "rtsp://u:p@203.0.113.20/2") == 0);
    krtsp_config_free(config);

    CHECK(write_config("main_only.conf",
        "[camera \"single\"]\n"
        "main = rtsp://u:p@203.0.113.21/1\n", 0600));
    CHECK(krtsp_config_load(&config, temp_path("main_only.conf"), error,
                            sizeof(error)));
    camera = krtsp_config_find(config, "single");
    CHECK(strcmp(krtsp_camera_url(camera, KRTSP_TIER_SUB),
                 "rtsp://u:p@203.0.113.21/1") == 0);
    CHECK(krtsp_camera_url(camera, (krtsp_tier)99) == NULL);
    krtsp_config_free(config);

    CHECK(krtsp_camera_url(NULL, KRTSP_TIER_SUB) == NULL);
    return true;
}

static bool
test_refuses_world_readable(void)
{
    krtsp_config *config = NULL;
    char error[256];

    /* This file holds passwords.  Loading it anyway with a warning would
     * defeat the reason it lives outside the repository. */
    CHECK(write_config("loose.conf",
        "[camera \"a\"]\nsub = rtsp://u:p@203.0.113.30/1\n", 0644));
    CHECK(!krtsp_config_load(&config, temp_path("loose.conf"), error,
                             sizeof(error)));
    CHECK(config == NULL);
    CHECK(strstr(error, "readable by others") != NULL);
    /* The message must be actionable, not just disapproving. */
    CHECK(strstr(error, "chmod 600") != NULL);

    /* Group-readable is refused too, not only world-readable. */
    CHECK(write_config("group.conf",
        "[camera \"a\"]\nsub = rtsp://u:p@203.0.113.30/1\n", 0640));
    CHECK(!krtsp_config_load(&config, temp_path("group.conf"), error,
                             sizeof(error)));
    return true;
}

static bool
test_errors_never_leak_a_url(void)
{
    krtsp_config *config = NULL;
    char error[256];

    /* A parse error that helpfully echoed the offending line would put a
     * password into whatever logs it. */
    CHECK(write_config("bad_key.conf",
        "[camera \"a\"]\n"
        "mian = rtsp://secretuser:secretpass@203.0.113.40/1\n", 0600));
    CHECK(!krtsp_config_load(&config, temp_path("bad_key.conf"), error,
                             sizeof(error)));
    CHECK(strstr(error, "secretpass") == NULL);
    CHECK(strstr(error, "203.0.113.40") == NULL);
    CHECK(strstr(error, "rtsp://") == NULL);
    CHECK(strstr(error, "mian") != NULL);   /* the key itself is safe to name */

    /* Same for an over-long URL, where the value is the problem. */
    {
        char line[1200];
        char body[1400];

        memset(line, 'x', sizeof(line) - 1u);
        line[sizeof(line) - 1u] = '\0';
        (void)snprintf(body, sizeof(body),
                       "[camera \"a\"]\nsub = rtsp://u:p@%s/1\n", line);
        CHECK(write_config("long.conf", body, 0600));
        CHECK(!krtsp_config_load(&config, temp_path("long.conf"), error,
                                 sizeof(error)));
        CHECK(strstr(error, "xxxx") == NULL);
    }
    return true;
}

static bool
test_structural_errors(void)
{
    krtsp_config *config = NULL;
    char error[256];

    /* A key before any section. */
    CHECK(write_config("orphan.conf", "sub = rtsp://u:p@203.0.113.50/1\n",
                       0600));
    CHECK(!krtsp_config_load(&config, temp_path("orphan.conf"), error,
                             sizeof(error)));
    CHECK(strstr(error, "before any section") != NULL);

    /* Duplicate camera names would make find() ambiguous. */
    CHECK(write_config("dupe.conf",
        "[camera \"a\"]\nsub = rtsp://u:p@203.0.113.51/1\n"
        "[camera \"a\"]\nsub = rtsp://u:p@203.0.113.52/1\n", 0600));
    CHECK(!krtsp_config_load(&config, temp_path("dupe.conf"), error,
                             sizeof(error)));
    CHECK(strstr(error, "duplicate") != NULL);

    /* A camera with no URL at all is a typo that would otherwise surface
     * much later as a source that never starts. */
    CHECK(write_config("empty.conf", "[camera \"a\"]\n", 0600));
    CHECK(!krtsp_config_load(&config, temp_path("empty.conf"), error,
                             sizeof(error)));
    CHECK(strstr(error, "neither a main nor a sub") != NULL);

    /* A group naming a camera that does not exist, likewise: better here
     * than as a permanently empty tile. */
    CHECK(write_config("badgroup.conf",
        "[camera \"a\"]\nsub = rtsp://u:p@203.0.113.53/1\n"
        "[group \"g\"]\ncameras = a, ghost\n", 0600));
    CHECK(!krtsp_config_load(&config, temp_path("badgroup.conf"), error,
                             sizeof(error)));
    CHECK(strstr(error, "ghost") != NULL);

    /* Malformed section header. */
    CHECK(write_config("badsection.conf", "[camera]\n", 0600));
    CHECK(!krtsp_config_load(&config, temp_path("badsection.conf"), error,
                             sizeof(error)));

    /* Unknown section type. */
    CHECK(write_config("unknown.conf", "[widget \"w\"]\nx = 1\n", 0600));
    CHECK(!krtsp_config_load(&config, temp_path("unknown.conf"), error,
                             sizeof(error)));
    CHECK(strstr(error, "unknown section") != NULL);

    /* Missing file. */
    CHECK(!krtsp_config_load(&config, temp_path("nothing-here.conf"), error,
                             sizeof(error)));
    return true;
}

static bool
test_strict_sections_and_duplicates(void)
{
    CHECK(load_fails("section-trailing.conf",
        "[camera \"a\"] trailing\nsub = rtsp://example/1\n", "line 1"));
    CHECK(load_fails("section-inner.conf",
        "[camera \"a\" junk]\nsub = rtsp://example/1\n", "line 1"));
    CHECK(load_fails("duplicate-group.conf",
        "[camera \"a\"]\nsub = rtsp://example/1\n"
        "[group \"g\"]\ncameras = a\n"
        "[group \"g\"]\ncameras = a\n", "duplicate"));
    CHECK(load_fails("cross-duplicate.conf",
        "[camera \"same\"]\nsub = rtsp://example/1\n"
        "[group \"same\"]\ncameras = same\n", "duplicate"));
    CHECK(load_fails("duplicate-url.conf",
        "[camera \"a\"]\nsub = rtsp://example/1\n"
        "sub = rtsp://example/2\n", "duplicate sub"));
    CHECK(load_fails("duplicate-member.conf",
        "[camera \"a\"]\nsub = rtsp://example/1\n"
        "[group \"g\"]\ncameras = a, a\n", "duplicate camera"));
    CHECK(load_fails("empty-member.conf",
        "[camera \"a\"]\nsub = rtsp://example/1\n"
        "[group \"g\"]\ncameras = a,,a\n", "invalid camera"));
    CHECK(load_fails("empty-group.conf",
        "[camera \"a\"]\nsub = rtsp://example/1\n"
        "[group \"g\"]\n", "has no cameras"));
    return true;
}

static bool
test_text_input_is_bounded_and_safe(void)
{
    static const unsigned char binary[] = {
        '[', 'c', 'a', 'm', 'e', 'r', 'a', ' ', '"', 'a', '"', ']', '\n',
        's', 'u', 'b', ' ', '=', ' ', 'r', 't', 's', 'p', ':', '/', '/',
        'x', '\0', 'y', '\n'
    };
    krtsp_config *config = NULL;
    char error[256];
    char long_line[1200];
    char body[1400];

    memset(long_line, 'x', sizeof(long_line) - 1u);
    long_line[sizeof(long_line) - 1u] = '\0';
    CHECK(snprintf(body, sizeof(body), "[camera \"a\"]\n%s\n", long_line) > 0);
    CHECK(load_fails("bounded-line.conf", body, "longer than"));

    CHECK(write_binary_config("binary.conf", binary, sizeof(binary), 0600));
    CHECK(!krtsp_config_load(&config, temp_path("binary.conf"), error,
                             sizeof(error)));
    CHECK(config == NULL);
    CHECK(strstr(error, "NUL byte") != NULL);

    CHECK(load_fails("control-name.conf",
        "[camera \"bad\033name\"]\nsub = rtsp://example/1\n", "line 1"));
    CHECK(load_fails("control-value.conf",
        "[camera \"a\"]\nsub = rtsp://example/\033[31m\n", "control"));
    return true;
}

static bool
test_file_type_and_symlink_rejections(void)
{
    krtsp_config *config = NULL;
    char target[640];
    char link_path[640];
    char fifo_path[640];
    char error[256];

    CHECK(write_config("link-target.conf",
        "[camera \"a\"]\nsub = rtsp://example/1\n", 0600));
    CHECK(snprintf(target, sizeof(target), "%s/link-target.conf",
                   scratch_dir) > 0);
    CHECK(snprintf(link_path, sizeof(link_path), "%s/link.conf",
                   scratch_dir) > 0);
    CHECK(symlink(target, link_path) == 0);
    CHECK(!krtsp_config_load(&config, link_path, error, sizeof(error)));
    CHECK(config == NULL);
    CHECK(strstr(error, "symbolic link") != NULL);

    CHECK(snprintf(fifo_path, sizeof(fifo_path), "%s/config.fifo",
                   scratch_dir) > 0);
    CHECK(mkfifo(fifo_path, 0600) == 0);
    /* O_NONBLOCK makes this a rejection, not a wait for a FIFO writer. */
    CHECK(!krtsp_config_load(&config, fifo_path, error, sizeof(error)));
    CHECK(strstr(error, "not a regular file") != NULL);
    return true;
}

typedef struct load_thread {
    const char *path;
    int failed;
} load_thread;

static void *load_thread_main(void *argument)
{
    load_thread *state = argument;

    for (int iteration = 0; iteration < 100; ++iteration) {
        krtsp_config *config = NULL;
        char error[128];

        if (!krtsp_config_load(&config, state->path, error, sizeof(error)) ||
            krtsp_config_group_count(config) != 1u) {
            state->failed = 1;
        }
        krtsp_config_free(config);
    }
    return NULL;
}

static bool
test_concurrent_loads_are_independent(void)
{
    char path[640];
    pthread_t threads[8];
    load_thread states[8];

    CHECK(write_config("concurrent.conf",
        "[camera \"a\"]\nsub = rtsp://example/1\n"
        "[camera \"b\"]\nsub = rtsp://example/2\n"
        "[group \"g\"]\ncameras = a, b\n", 0600));
    CHECK(snprintf(path, sizeof(path), "%s/concurrent.conf", scratch_dir) > 0);
    for (size_t index = 0u; index < 8u; ++index) {
        states[index].path = path;
        states[index].failed = 0;
        CHECK(pthread_create(&threads[index], NULL, load_thread_main,
                             &states[index]) == 0);
    }
    for (size_t index = 0u; index < 8u; ++index) {
        CHECK(pthread_join(threads[index], NULL) == 0);
        CHECK(states[index].failed == 0);
    }
    return true;
}

static bool
test_url_with_equals_survives(void)
{
    krtsp_config *config = NULL;
    char error[256];
    const krtsp_camera *camera;

    /* Splitting "key = value" on the last '=' would truncate a query
     * string, which cameras do use. */
    CHECK(write_config("query.conf",
        "[camera \"q\"]\n"
        "sub = rtsp://u:p@203.0.113.60/live?channel=1&subtype=0\n", 0600));
    CHECK(krtsp_config_load(&config, temp_path("query.conf"), error,
                            sizeof(error)));
    camera = krtsp_config_find(config, "q");
    CHECK(camera != NULL);
    CHECK(strcmp(krtsp_camera_url(camera, KRTSP_TIER_SUB),
                 "rtsp://u:p@203.0.113.60/live?channel=1&subtype=0") == 0);
    krtsp_config_free(config);
    return true;
}

static bool
test_paths(void)
{
    char root[512];
    char config_dir[512];
    struct stat info;

    /* KILIX_RTSP_HOME keeps tests out of the real state directory. */
    CHECK(krtsp_paths_dir(NULL, root, sizeof(root)));
    CHECK(stat(root, &info) == 0 && S_ISDIR(info.st_mode));

    CHECK(krtsp_paths_dir("config", config_dir, sizeof(config_dir)));
    CHECK(stat(config_dir, &info) == 0 && S_ISDIR(info.st_mode));
    /* Created 0700: it sits next to a file full of passwords. */
    CHECK((info.st_mode & (S_IRWXG | S_IRWXO)) == 0);
    CHECK(strstr(config_dir, "/config") != NULL);

    /* Idempotent. */
    CHECK(krtsp_paths_dir("config", config_dir, sizeof(config_dir)));

    /* Creation modes are exact even when the calling process has a umask
     * stricter than the ordinary private 0077. */
    {
        char strict_root[640];
        char strict_config[640];
        mode_t previous;
        bool created;

        CHECK(snprintf(strict_root, sizeof(strict_root), "%s/strict",
                       scratch_dir) > 0);
        (void)setenv("KILIX_RTSP_HOME", strict_root, 1);
        previous = umask(0777);
        created = krtsp_paths_dir("config", strict_config,
                                  sizeof(strict_config));
        (void)umask(previous);
        (void)setenv("KILIX_RTSP_HOME", scratch_dir, 1);
        CHECK(created);
        CHECK(stat(strict_root, &info) == 0 &&
              (info.st_mode & 0777) == 0700);
        CHECK(stat(strict_config, &info) == 0 &&
              (info.st_mode & 0777) == 0700);
    }

    /* A leaf is never a path. */
    CHECK(!krtsp_paths_dir("../escape", config_dir, sizeof(config_dir)));
    CHECK(!krtsp_paths_dir("a/b", config_dir, sizeof(config_dir)));
    CHECK(!krtsp_paths_dir(".", config_dir, sizeof(config_dir)));
    CHECK(!krtsp_paths_dir("has space", config_dir, sizeof(config_dir)));
    CHECK(!krtsp_paths_dir("has\\backslash", config_dir,
                           sizeof(config_dir)));

    /* Existing component directories must retain the 0700 boundary. */
    {
        char public_dir[640];
        char link_root[640];

        CHECK(snprintf(public_dir, sizeof(public_dir), "%s/public",
                       scratch_dir) > 0);
        CHECK(mkdir(public_dir, 0700) == 0);
        CHECK(chmod(public_dir, 0755) == 0);
        CHECK(!krtsp_paths_dir("public", config_dir, sizeof(config_dir)));
        CHECK(errno == EACCES);

        CHECK(chmod(scratch_dir, 0755) == 0);
        CHECK(!krtsp_paths_dir(NULL, root, sizeof(root)));
        CHECK(errno == EACCES);
        CHECK(chmod(scratch_dir, 0700) == 0);

        CHECK(snprintf(link_root, sizeof(link_root), "%s/root-link",
                       scratch_dir) > 0);
        CHECK(symlink(scratch_dir, link_root) == 0);
        (void)setenv("KILIX_RTSP_HOME", link_root, 1);
        CHECK(!krtsp_paths_dir(NULL, root, sizeof(root)));
        (void)setenv("KILIX_RTSP_HOME", scratch_dir, 1);
    }

    /* Too small a buffer fails rather than truncating into a wrong path. */
    {
        char tiny[4];

        CHECK(!krtsp_paths_dir("config", tiny, sizeof(tiny)));
    }
    CHECK(!krtsp_paths_dir(NULL, NULL, 0u));
    return true;
}

typedef bool (*test_function)(void);

typedef struct test_case {
    const char *name;
    test_function function;
} test_case;

int
main(void)
{
    static const test_case tests[] = {
        {"parses cameras and groups", test_parses_cameras_and_groups},
        {"tier fallback", test_tier_fallback},
        {"refuses world readable", test_refuses_world_readable},
        {"errors never leak a url", test_errors_never_leak_a_url},
        {"structural errors", test_structural_errors},
        {"strict sections and duplicates",
         test_strict_sections_and_duplicates},
        {"text input is bounded and safe", test_text_input_is_bounded_and_safe},
        {"file type and symlink rejections",
         test_file_type_and_symlink_rejections},
        {"concurrent loads are independent",
         test_concurrent_loads_are_independent},
        {"url with equals survives", test_url_with_equals_survives},
        {"paths", test_paths}
    };
    size_t passed = 0u;
    char template[] = "/tmp/krtsp-test-XXXXXX";

    if (mkdtemp(template) == NULL) {
        (void)fprintf(stderr, "cannot create a scratch directory\n");
        return 1;
    }
    (void)snprintf(scratch_dir, sizeof(scratch_dir), "%s", template);
    /* Never touch the real state directory from a test. */
    (void)setenv("KILIX_RTSP_HOME", scratch_dir, 1);

    for (size_t index = 0u; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        const bool ok = tests[index].function();

        (void)printf("%s %s\n", ok ? "ok" : "not ok", tests[index].name);
        if (!ok) {
            return 1;
        }
        ++passed;
    }
    (void)printf("%zu tests passed\n", passed);
    return 0;
}
