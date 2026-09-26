#include "kilix_rtsp.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

#define GIB ((uint64_t)1024u * 1024u * 1024u)
#define MIB ((uint64_t)1024u * 1024u)

typedef struct test_case {
    const char *name;
    bool (*function)(void);
} test_case;

static bool parses_as(const char *text, krtsp_buffer_kind kind,
                      uint64_t amount)
{
    krtsp_buffer_spec spec;

    if (!krtsp_buffer_parse(text, &spec)) {
        (void)fprintf(stderr, "'%s' did not parse\n", text);
        return false;
    }
    if (spec.kind != kind || spec.amount != amount) {
        (void)fprintf(stderr, "'%s' parsed as kind %d amount %llu\n", text,
                      (int)spec.kind, (unsigned long long)spec.amount);
        return false;
    }
    return true;
}

static bool refused(const char *text)
{
    krtsp_buffer_spec spec;

    if (krtsp_buffer_parse(text, &spec)) {
        (void)fprintf(stderr, "'%s' should not have parsed\n", text);
        return false;
    }
    return true;
}

static bool test_parse_accepts_sizes_and_durations(void)
{
    CHECK(parses_as("auto", KRTSP_BUFFER_AUTO, 0u));
    CHECK(parses_as("off", KRTSP_BUFFER_OFF, 0u));
    CHECK(parses_as("0", KRTSP_BUFFER_OFF, 0u));
    CHECK(parses_as("500M", KRTSP_BUFFER_BYTES, 500u * MIB));
    CHECK(parses_as("2G", KRTSP_BUFFER_BYTES, 2u * GIB));
    CHECK(parses_as("2GB", KRTSP_BUFFER_BYTES, 2u * GIB));
    CHECK(parses_as("1.5GiB", KRTSP_BUFFER_BYTES, GIB + GIB / 2u));
    CHECK(parses_as("64k", KRTSP_BUFFER_BYTES, 64u * 1024u));
    CHECK(parses_as("90s", KRTSP_BUFFER_SECONDS, 90u));
    CHECK(parses_as("10min", KRTSP_BUFFER_SECONDS, 600u));
    CHECK(parses_as("2h", KRTSP_BUFFER_SECONDS, 7200u));
    CHECK(parses_as("0.5h", KRTSP_BUFFER_SECONDS, 1800u));
    return true;
}

static bool test_parse_refuses_the_ambiguous_and_the_absurd(void)
{
    /* "10m" is ten minutes to one reader and ten megabytes to another;
     * a buffer sized by a guess is a disk filled by one. */
    CHECK(refused("10m"));
    CHECK(refused("10"));
    CHECK(refused("2"));
    CHECK(refused("G"));
    CHECK(refused("-5G"));
    CHECK(refused("abc"));
    CHECK(refused("5 parsecs"));
    CHECK(refused("2G extra"));
    CHECK(refused("1e30G"));
    CHECK(refused("nan"));
    CHECK(!krtsp_buffer_parse(NULL, &(krtsp_buffer_spec){0}));
    /* A capital M is unambiguous: it is bytes. */
    CHECK(parses_as("10M", KRTSP_BUFFER_BYTES, 10u * MIB));
    return true;
}

static krtsp_buffer_plan plan_for(krtsp_buffer_kind kind, uint64_t amount,
                                  uint64_t free_bytes, uint64_t total)
{
    krtsp_buffer_spec spec = {kind, amount};
    krtsp_buffer_plan plan;

    krtsp_buffer_plan_make(&spec, free_bytes, total, &plan);
    return plan;
}

static bool test_default_follows_the_disk(void)
{
    /* 100 GiB disk, 60 free: reserve is 5% = 5 GiB, a tenth of free is
     * 6 GiB, and that fits. */
    krtsp_buffer_plan plan = plan_for(KRTSP_BUFFER_AUTO, 0u, 60u * GIB,
                                      100u * GIB);

    CHECK(plan.enabled);
    CHECK(plan.max_bytes == 6u * GIB);
    CHECK(plan.max_seconds == 0u);
    CHECK(plan.reserve_bytes == 5u * GIB);
    CHECK(!plan.clamped);

    /* A huge, empty disk does not hand a buffer the whole afternoon. */
    plan = plan_for(KRTSP_BUFFER_AUTO, 0u, 4000u * GIB, 8000u * GIB);
    CHECK(plan.enabled && plan.max_bytes == 8u * GIB);

    plan = plan_for(KRTSP_BUFFER_AUTO, 0u, 3u * GIB, 100u * GIB);
    /* Free space is below the reserve: nothing here may be taken. */
    CHECK(!plan.enabled);

    plan = plan_for(KRTSP_BUFFER_AUTO, 0u, 6u * GIB, 100u * GIB);
    /* Reserve 5 GiB leaves 1 GiB to give; a tenth of free is less. */
    CHECK(plan.enabled && plan.max_bytes == 6u * GIB / 10u);
    /* When a tenth would exceed what may be given, the give is the cap. */
    plan = plan_for(KRTSP_BUFFER_AUTO, 0u, 5u * GIB + 300u * MIB, 100u * GIB);
    CHECK(plan.enabled && plan.max_bytes == 300u * MIB);
    return true;
}

static bool test_the_reserve_is_never_touched(void)
{
    /* Whatever is asked, free space after a full buffer still holds the
     * reserve. */
    static const uint64_t asks[] = {1u * GIB, 10u * GIB, 500u * GIB};

    for (size_t index = 0u; index < sizeof(asks) / sizeof(asks[0]); ++index) {
        krtsp_buffer_plan plan = plan_for(KRTSP_BUFFER_BYTES, asks[index],
                                          50u * GIB, 200u * GIB);

        CHECK(plan.enabled);
        CHECK(50u * GIB - plan.max_bytes >= plan.reserve_bytes);
    }
    /* 200 GiB disk: reserve 10 GiB; 50 free leaves 40 to give. */
    CHECK(plan_for(KRTSP_BUFFER_BYTES, 500u * GIB, 50u * GIB,
                   200u * GIB).max_bytes == 40u * GIB);
    CHECK(plan_for(KRTSP_BUFFER_BYTES, 500u * GIB, 50u * GIB,
                   200u * GIB).clamped);
    /* An ask that fits is not reported as cut. */
    CHECK(!plan_for(KRTSP_BUFFER_BYTES, 1u * GIB, 50u * GIB,
                    200u * GIB).clamped);
    return true;
}

static bool test_off_and_too_small_do_nothing(void)
{
    CHECK(!plan_for(KRTSP_BUFFER_OFF, 0u, 100u * GIB, 100u * GIB).enabled);
    /* Room for less than a couple of segments is not a history. */
    CHECK(!plan_for(KRTSP_BUFFER_BYTES, 10u * MIB, 100u * GIB,
                    100u * GIB).enabled);
    CHECK(!plan_for(KRTSP_BUFFER_AUTO, 0u, 0u, 0u).enabled);
    return true;
}

static bool test_a_duration_keeps_a_disk_ceiling(void)
{
    krtsp_buffer_plan plan = plan_for(KRTSP_BUFFER_SECONDS, 600u, 60u * GIB,
                                      100u * GIB);

    CHECK(plan.enabled);
    CHECK(plan.max_seconds == 600u);
    /* Ten minutes of a high-bitrate camera must still not outrun the disk. */
    CHECK(plan.max_bytes == 6u * GIB);
    return true;
}

/* ------------------------------ segments -------------------------------- */

static char scratch[512];

static bool make_scratch(void)
{
    (void)snprintf(scratch, sizeof(scratch), "/tmp/krtsp-buffer.XXXXXX");
    return mkdtemp(scratch) != NULL;
}

static bool put(const char *name, size_t bytes)
{
    char path[640];
    FILE *file;

    (void)snprintf(path, sizeof(path), "%s/%s", scratch, name);
    file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    if (bytes > 0u && fseek(file, (long)bytes - 1, SEEK_SET) == 0) {
        (void)fputc(0, file);
    }
    return fclose(file) == 0;
}

static bool exists(const char *name)
{
    char path[640];
    struct stat info;

    (void)snprintf(path, sizeof(path), "%s/%s", scratch, name);
    return stat(path, &info) == 0;
}

static void clear_scratch(void)
{
    krtsp_buffer_dir_remove(scratch);
}

static bool test_scan_orders_and_ignores_the_rest(void)
{
    krtsp_segment found[8];
    size_t count;

    CHECK(make_scratch());
    CHECK(put("2026-09-25_10.00.20.mkv", 100u));
    CHECK(put("2026-09-25_10.00.00.mkv", 300u));
    CHECK(put("2026-09-25_10.00.10.mkv", 200u));
    CHECK(put(".segments", 50u));
    CHECK(put("notes.txt", 10u));
    CHECK(put("2026-09-25_10.00.30.mp4", 10u));   /* wrong container */

    count = krtsp_segments_scan(scratch, found, 8u);
    CHECK(count == 3u);
    CHECK(strcmp(found[0].name, "2026-09-25_10.00.00.mkv") == 0);
    CHECK(strcmp(found[2].name, "2026-09-25_10.00.20.mkv") == 0);
    CHECK(found[0].bytes == 300u && found[1].bytes == 200u);
    CHECK(found[1].start - found[0].start == 10);
    CHECK(found[2].start - found[1].start == 10);

    /* Too small a table keeps the newest, not the oldest. */
    count = krtsp_segments_scan(scratch, found, 2u);
    CHECK(count == 2u);
    CHECK(strcmp(found[0].name, "2026-09-25_10.00.10.mkv") == 0);
    CHECK(krtsp_segments_scan("/nonexistent-krtsp", found, 8u) == 0u);
    clear_scratch();
    return true;
}

static bool test_prune_by_bytes_keeps_the_live_edge(void)
{
    krtsp_buffer_plan plan = {true, 350u, 0u, 0u, false};
    krtsp_segment found[8];

    CHECK(make_scratch());
    CHECK(put("2026-09-25_10.00.00.mkv", 300u));
    CHECK(put("2026-09-25_10.00.10.mkv", 200u));
    CHECK(put("2026-09-25_10.00.20.mkv", 100u));
    CHECK(put("2026-09-25_10.00.30.mkv", 100u));

    /* 700 bytes against a 350 ceiling: the oldest go until it fits. */
    CHECK(krtsp_segments_prune(scratch, &plan, 0, 2u) == 2u);
    CHECK(!exists("2026-09-25_10.00.00.mkv"));
    CHECK(!exists("2026-09-25_10.00.10.mkv"));
    CHECK(exists("2026-09-25_10.00.20.mkv"));
    CHECK(exists("2026-09-25_10.00.30.mkv"));

    /* Even a ceiling below the newest two leaves them alone. */
    plan.max_bytes = 1u;
    CHECK(krtsp_segments_prune(scratch, &plan, 0, 2u) == 0u);
    CHECK(krtsp_segments_scan(scratch, found, 8u) == 2u);

    /* And a disabled plan deletes nothing at all. */
    plan.enabled = false;
    CHECK(krtsp_segments_prune(scratch, &plan, 0, 0u) == 0u);
    clear_scratch();
    return true;
}

static time_t start_of(const char *name)
{
    krtsp_segment found[8];
    size_t count = krtsp_segments_scan(scratch, found, 8u);

    for (size_t index = 0u; index < count; ++index) {
        if (strcmp(found[index].name, name) == 0) {
            return found[index].start;
        }
    }
    return 0;
}

static bool test_prune_by_age(void)
{
    krtsp_buffer_plan plan = {true, 1000000u, 25u, 0u, false};
    time_t now;

    CHECK(make_scratch());
    CHECK(put("2026-09-25_10.00.00.mkv", 10u));
    CHECK(put("2026-09-25_10.00.10.mkv", 10u));
    CHECK(put("2026-09-25_10.00.20.mkv", 10u));
    CHECK(put("2026-09-25_10.00.30.mkv", 10u));
    CHECK(put("2026-09-25_10.00.40.mkv", 10u));
    now = start_of("2026-09-25_10.00.40.mkv") + 5;

    /* 25 seconds of history: anything that started more than 25 s ago
     * is gone, which is the first two. */
    CHECK(krtsp_segments_prune(scratch, &plan, now, 2u) == 2u);
    CHECK(!exists("2026-09-25_10.00.00.mkv"));
    CHECK(!exists("2026-09-25_10.00.10.mkv"));
    CHECK(exists("2026-09-25_10.00.20.mkv"));
    clear_scratch();
    return true;
}

static bool test_locate_finds_the_segment_and_the_offset(void)
{
    krtsp_segment found[4];
    size_t index = 99u;
    int offset = -1;
    time_t first;

    CHECK(make_scratch());
    CHECK(put("2026-09-25_10.00.00.mkv", 1u));
    CHECK(put("2026-09-25_10.00.10.mkv", 1u));
    CHECK(put("2026-09-25_10.00.20.mkv", 1u));
    CHECK(krtsp_segments_scan(scratch, found, 4u) == 3u);
    first = found[0].start;

    CHECK(krtsp_segments_locate(found, 3u, first + 14, &index, &offset));
    CHECK(index == 1u && offset == 4);
    CHECK(krtsp_segments_locate(found, 3u, first + 10, &index, &offset));
    CHECK(index == 1u && offset == 0);
    /* After the newest starts: inside the newest, however far. */
    CHECK(krtsp_segments_locate(found, 3u, first + 400, &index, &offset));
    CHECK(index == 2u && offset == 380);
    /* Before the oldest: clamped to its start, and it says so. */
    CHECK(!krtsp_segments_locate(found, 3u, first - 5, &index, &offset));
    CHECK(index == 0u && offset == 0);
    /* Nothing to locate in. */
    index = 7u;
    CHECK(!krtsp_segments_locate(found, 0u, first, &index, &offset));
    CHECK(index == 7u);
    clear_scratch();
    return true;
}

static bool test_directories_are_private_per_process_and_swept(void)
{
    char home[256];
    char dir[512];
    char stale[600];
    char again[512];
    pid_t child;
    int status = 0;
    char *saved = getenv("KILIX_RTSP_HOME");
    char saved_copy[512] = "";

    if (saved != NULL) {
        (void)snprintf(saved_copy, sizeof(saved_copy), "%s", saved);
    }
    (void)snprintf(home, sizeof(home), "/tmp/krtsp-home.XXXXXX");
    CHECK(mkdtemp(home) != NULL);
    CHECK(chmod(home, 0700) == 0);
    CHECK(setenv("KILIX_RTSP_HOME", home, 1) == 0);

    CHECK(krtsp_buffer_dir_make("front door/../x", dir, sizeof(dir)));
    /* Path separators in a camera name cannot climb out. */
    CHECK(strstr(dir, "front_door_.._x.") != NULL);
    CHECK(strstr(dir, "/cache/buffer/") != NULL);
    {
        char expected[32];

        (void)snprintf(expected, sizeof(expected), ".%ld", (long)getpid());
        CHECK(strstr(dir, expected) != NULL);
    }
    /* Making it again is the same directory, not an error. */
    CHECK(krtsp_buffer_dir_make("front door/../x", again, sizeof(again)));
    CHECK(strcmp(dir, again) == 0);
    CHECK(krtsp_buffer_dir_make("..", again, sizeof(again)));
    CHECK(strstr(again, "/camera.") != NULL);

    /* A directory from a process that is gone is swept; ours is not. */
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child);
    {
        char root[512];
        char *slash = strrchr(dir, '/');

        CHECK(slash != NULL);
        (void)snprintf(root, sizeof(root), "%.*s", (int)(slash - dir), dir);
        (void)snprintf(stale, sizeof(stale), "%s/old.%ld", root, (long)child);
    }
    CHECK(mkdir(stale, 0700) == 0);
    {
        char leftover[700];
        FILE *file;

        (void)snprintf(leftover, sizeof(leftover), "%s/2026-09-25_10.00.00.mkv",
                       stale);
        file = fopen(leftover, "wb");
        CHECK(file != NULL);
        (void)fclose(file);
    }
    CHECK(krtsp_buffer_sweep() == 1u);
    {
        struct stat info;

        CHECK(stat(stale, &info) != 0);
        CHECK(stat(dir, &info) == 0);
    }

    krtsp_buffer_dir_remove(dir);
    {
        struct stat info;

        CHECK(stat(dir, &info) != 0);
    }
    krtsp_buffer_dir_remove(NULL);
    if (saved != NULL) {
        (void)setenv("KILIX_RTSP_HOME", saved_copy, 1);
    } else {
        (void)unsetenv("KILIX_RTSP_HOME");
    }
    return true;
}

static bool test_the_disk_can_be_read(void)
{
    uint64_t free_bytes = 0u;
    uint64_t total = 0u;
    char text[32];

    CHECK(krtsp_buffer_disk("/tmp", &free_bytes, &total));
    CHECK(total > 0u && free_bytes <= total);
    CHECK(!krtsp_buffer_disk("/nonexistent-krtsp", &free_bytes, &total));
    krtsp_buffer_format_bytes(1536u * MIB, text, sizeof(text));
    CHECK(strcmp(text, "1.5 GB") == 0);
    krtsp_buffer_format_bytes(42u * MIB, text, sizeof(text));
    CHECK(strcmp(text, "42 MB") == 0);
    return true;
}

int
main(void)
{
    static const test_case tests[] = {
        {"parse accepts sizes and durations",
         test_parse_accepts_sizes_and_durations},
        {"parse refuses the ambiguous and the absurd",
         test_parse_refuses_the_ambiguous_and_the_absurd},
        {"default follows the disk", test_default_follows_the_disk},
        {"the reserve is never touched", test_the_reserve_is_never_touched},
        {"off and too small do nothing", test_off_and_too_small_do_nothing},
        {"a duration keeps a disk ceiling",
         test_a_duration_keeps_a_disk_ceiling},
        {"scan orders and ignores the rest",
         test_scan_orders_and_ignores_the_rest},
        {"prune by bytes keeps the live edge",
         test_prune_by_bytes_keeps_the_live_edge},
        {"prune by age", test_prune_by_age},
        {"locate finds the segment and the offset",
         test_locate_finds_the_segment_and_the_offset},
        {"directories are private per process and swept",
         test_directories_are_private_per_process_and_swept},
        {"the disk can be read", test_the_disk_can_be_read}
    };
    size_t passed = 0u;

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
