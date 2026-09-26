/*
 * The history buffer: sizing, the segment set, and where it lives.
 *
 * Everything here is arithmetic over numbers and file names, with the disk
 * as the only outside input, so the parts that decide how much a viewer may
 * take from a machine are testable without a camera.
 */

/* strptime() is XSI, not POSIX.1-2008 base: the segment names are parsed
 * with the same format the recorder wrote them with. */
#define _XOPEN_SOURCE 700

#include "kilix_rtsp.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define KIB ((uint64_t)1024u)
#define MIB (KIB * 1024u)
#define GIB (MIB * 1024u)

/* The default is a tenth of what is free, but never less than this (a
 * buffer smaller than a few segments is not a history) or more than that
 * (a day of one camera is not what somebody who typed nothing wanted). */
#define AUTO_FLOOR (256u * MIB)
#define AUTO_CEILING (8u * GIB)
/* Below this there is no point starting: a couple of segments at most. */
#define USEFUL_MINIMUM (64u * MIB)
#define RESERVE_FLOOR (2u * GIB)

/* The recorder's default pattern; see krtsp_args.c. */
#define SEGMENT_FORMAT "%Y-%m-%d_%H.%M.%S.mkv"

/* ------------------------------ parsing --------------------------------- */

static bool multiply_checked(double value, uint64_t unit, uint64_t *out)
{
    double product = value * (double)unit;

    if (!(product >= 0.0) || product >= 1.8e19) {
        return false;
    }
    *out = (uint64_t)product;
    return true;
}

bool krtsp_buffer_parse(const char *text, krtsp_buffer_spec *out)
{
    char *end = NULL;
    double value;
    char unit[8];
    size_t length = 0u;

    if (text == NULL || out == NULL) {
        return false;
    }
    out->kind = KRTSP_BUFFER_AUTO;
    out->amount = 0u;

    if (strcmp(text, "auto") == 0 || strcmp(text, "") == 0) {
        return true;
    }
    if (strcmp(text, "off") == 0 || strcmp(text, "none") == 0 ||
        strcmp(text, "0") == 0) {
        out->kind = KRTSP_BUFFER_OFF;
        return true;
    }

    errno = 0;
    value = strtod(text, &end);
    if (end == text || errno != 0 || !(value > 0.0) || value > 1e15) {
        return false;
    }
    while (*end == ' ') {
        ++end;
    }
    while (end[length] != '\0' && length < sizeof(unit) - 1u) {
        unit[length] = (char)tolower((unsigned char)end[length]);
        ++length;
    }
    unit[length] = '\0';
    if (end[length] != '\0' || length == 0u) {
        return false;   /* a bare number carries no meaning */
    }

    if (strcmp(unit, "k") == 0 || strcmp(unit, "kb") == 0 ||
        strcmp(unit, "kib") == 0) {
        out->kind = KRTSP_BUFFER_BYTES;
        return multiply_checked(value, KIB, &out->amount);
    }
    if (strcmp(unit, "m") == 0 && end[0] == 'M') {
        /* "M" is megabytes; a lowercase "m" is the ambiguous one. */
        out->kind = KRTSP_BUFFER_BYTES;
        return multiply_checked(value, MIB, &out->amount);
    }
    if (strcmp(unit, "mb") == 0 || strcmp(unit, "mib") == 0) {
        out->kind = KRTSP_BUFFER_BYTES;
        return multiply_checked(value, MIB, &out->amount);
    }
    if (strcmp(unit, "g") == 0 || strcmp(unit, "gb") == 0 ||
        strcmp(unit, "gib") == 0) {
        out->kind = KRTSP_BUFFER_BYTES;
        return multiply_checked(value, GIB, &out->amount);
    }
    if (strcmp(unit, "t") == 0 || strcmp(unit, "tb") == 0 ||
        strcmp(unit, "tib") == 0) {
        out->kind = KRTSP_BUFFER_BYTES;
        return multiply_checked(value, GIB * 1024u, &out->amount);
    }
    if (strcmp(unit, "s") == 0 || strcmp(unit, "sec") == 0) {
        out->kind = KRTSP_BUFFER_SECONDS;
        return multiply_checked(value, 1u, &out->amount);
    }
    if (strcmp(unit, "min") == 0 || strcmp(unit, "mins") == 0) {
        out->kind = KRTSP_BUFFER_SECONDS;
        return multiply_checked(value, 60u, &out->amount);
    }
    if (strcmp(unit, "h") == 0 || strcmp(unit, "hr") == 0 ||
        strcmp(unit, "hrs") == 0) {
        out->kind = KRTSP_BUFFER_SECONDS;
        return multiply_checked(value, 3600u, &out->amount);
    }
    out->kind = KRTSP_BUFFER_AUTO;
    out->amount = 0u;
    return false;   /* including a lowercase lone "m" */
}

/* ------------------------------- planning ------------------------------- */

static uint64_t reserve_for(uint64_t total_bytes)
{
    uint64_t five_percent = total_bytes / 20u;

    return five_percent > RESERVE_FLOOR ? five_percent : RESERVE_FLOOR;
}

void krtsp_buffer_plan_make(
    const krtsp_buffer_spec *spec, uint64_t free_bytes, uint64_t total_bytes,
    krtsp_buffer_plan *plan)
{
    uint64_t reserve = reserve_for(total_bytes);
    uint64_t available;
    uint64_t wanted;

    if (plan == NULL) {
        return;
    }
    (void)memset(plan, 0, sizeof(*plan));
    plan->reserve_bytes = reserve;
    if (spec == NULL || spec->kind == KRTSP_BUFFER_OFF) {
        return;
    }

    /* What this process may ever hold: whatever is free beyond the space
     * that has to stay free. */
    available = free_bytes > reserve ? free_bytes - reserve : 0u;

    if (spec->kind == KRTSP_BUFFER_BYTES) {
        wanted = spec->amount;
    } else {
        wanted = free_bytes / 10u;
        if (wanted < AUTO_FLOOR) {
            wanted = AUTO_FLOOR;
        }
        if (wanted > AUTO_CEILING) {
            wanted = AUTO_CEILING;
        }
    }
    if (wanted > available) {
        wanted = available;
        plan->clamped = spec->kind == KRTSP_BUFFER_BYTES;
    }
    if (wanted < USEFUL_MINIMUM) {
        return;   /* not enough room to be worth starting */
    }
    plan->enabled = true;
    plan->max_bytes = wanted;
    if (spec->kind == KRTSP_BUFFER_SECONDS) {
        plan->max_seconds = spec->amount;
    }
}

bool krtsp_buffer_disk(const char *path, uint64_t *free_bytes,
                       uint64_t *total_bytes)
{
    struct statvfs info;

    if (path == NULL || free_bytes == NULL || total_bytes == NULL ||
        statvfs(path, &info) != 0) {
        return false;
    }
    /* f_bavail is what an unprivileged writer can use, which is the
     * question; f_bfree includes blocks held back for root. */
    *free_bytes = (uint64_t)info.f_bavail * (uint64_t)info.f_frsize;
    *total_bytes = (uint64_t)info.f_blocks * (uint64_t)info.f_frsize;
    return true;
}

void krtsp_buffer_format_bytes(uint64_t bytes, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0u) {
        return;
    }
    if (bytes >= GIB) {
        (void)snprintf(out, capacity, "%.1f GB", (double)bytes / (double)GIB);
    } else if (bytes >= MIB) {
        (void)snprintf(out, capacity, "%.0f MB", (double)bytes / (double)MIB);
    } else {
        (void)snprintf(out, capacity, "%.0f KB", (double)bytes / (double)KIB);
    }
}

/* ------------------------------ segments -------------------------------- */

static bool segment_start(const char *name, time_t *start)
{
    struct tm parts;
    const char *rest;

    (void)memset(&parts, 0, sizeof(parts));
    rest = strptime(name, SEGMENT_FORMAT, &parts);
    if (rest == NULL || *rest != '\0') {
        return false;
    }
    /* The recorder wrote local time; let the C library decide whether
     * daylight saving applied then. */
    parts.tm_isdst = -1;
    *start = mktime(&parts);
    return *start != (time_t)-1;
}

static int compare_segments(const void *left, const void *right)
{
    const krtsp_segment *a = left;
    const krtsp_segment *b = right;

    if (a->start != b->start) {
        return a->start < b->start ? -1 : 1;
    }
    return strcmp(a->name, b->name);
}

/* The whole set, unbounded, so pruning can see every file. */
static krtsp_segment *scan_all(const char *dir, size_t *count)
{
    DIR *handle = opendir(dir);
    struct dirent *entry;
    krtsp_segment *found = NULL;
    size_t used = 0u;
    size_t room = 0u;

    *count = 0u;
    if (handle == NULL) {
        return NULL;
    }
    while ((entry = readdir(handle)) != NULL) {
        krtsp_segment segment;
        char path[PATH_MAX];
        struct stat info;
        time_t start;

        if (strlen(entry->d_name) >= sizeof(segment.name) ||
            !segment_start(entry->d_name, &start)) {
            continue;
        }
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >=
                (int)sizeof(path) ||
            lstat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
            continue;
        }
        (void)snprintf(segment.name, sizeof(segment.name), "%s",
                       entry->d_name);
        segment.start = start;
        segment.bytes = (uint64_t)info.st_size;
        if (used == room) {
            size_t bigger = room == 0u ? 64u : room * 2u;
            krtsp_segment *grown = realloc(found, bigger * sizeof(*found));

            if (grown == NULL) {
                break;
            }
            found = grown;
            room = bigger;
        }
        found[used++] = segment;
    }
    (void)closedir(handle);
    if (used > 1u) {
        qsort(found, used, sizeof(*found), compare_segments);
    }
    *count = used;
    return found;
}

size_t krtsp_segments_scan(const char *dir, krtsp_segment *out,
                           size_t capacity)
{
    size_t count = 0u;
    size_t skip;
    krtsp_segment *all;

    if (dir == NULL || out == NULL || capacity == 0u) {
        return 0u;
    }
    all = scan_all(dir, &count);
    if (all == NULL) {
        return 0u;
    }
    skip = count > capacity ? count - capacity : 0u;
    count -= skip;
    (void)memcpy(out, all + skip, count * sizeof(*out));
    free(all);
    return count;
}

size_t krtsp_segments_prune(
    const char *dir, const krtsp_buffer_plan *plan, time_t now,
    size_t keep_newest)
{
    size_t count = 0u;
    size_t removed = 0u;
    uint64_t total = 0u;
    krtsp_segment *all;

    if (dir == NULL || plan == NULL || !plan->enabled) {
        return 0u;
    }
    all = scan_all(dir, &count);
    if (all == NULL) {
        return 0u;
    }
    for (size_t index = 0u; index < count; ++index) {
        total += all[index].bytes;
    }
    for (size_t index = 0u; index + keep_newest < count; ++index) {
        bool too_big = total > plan->max_bytes;
        bool too_old = plan->max_seconds != 0u && now > all[index].start &&
                       (uint64_t)(now - all[index].start) > plan->max_seconds;
        char path[PATH_MAX];

        if (!too_big && !too_old) {
            break;   /* oldest first: nothing later is older or larger */
        }
        if (snprintf(path, sizeof(path), "%s/%s", dir, all[index].name) <
                (int)sizeof(path) &&
            unlink(path) == 0) {
            total -= all[index].bytes;
            ++removed;
        }
    }
    free(all);
    return removed;
}

bool krtsp_segments_locate(
    const krtsp_segment *segments, size_t count, time_t target,
    size_t *index, int *offset_seconds)
{
    size_t chosen = 0u;

    if (segments == NULL || count == 0u || index == NULL ||
        offset_seconds == NULL) {
        return false;
    }
    if (target < segments[0].start) {
        *index = 0u;
        *offset_seconds = 0;
        return false;
    }
    for (size_t at = 0u; at < count; ++at) {
        if (segments[at].start <= target) {
            chosen = at;
        } else {
            break;
        }
    }
    *index = chosen;
    *offset_seconds = (int)(target - segments[chosen].start);
    return true;
}

/* ------------------------------ directories ----------------------------- */

static bool buffer_root(char *out, size_t capacity)
{
    char cache[PATH_MAX];

    if (!krtsp_paths_dir("cache", cache, sizeof(cache))) {
        return false;
    }
    if (snprintf(out, capacity, "%s/buffer", cache) >= (int)capacity) {
        return false;
    }
    if (mkdir(out, 0700) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

bool krtsp_buffer_dir_make(const char *label, char *out, size_t capacity)
{
    char root[PATH_MAX];
    char clean[KRTSP_NAME_MAX];
    size_t length = 0u;

    if (label == NULL || out == NULL || !buffer_root(root, sizeof(root))) {
        return false;
    }
    for (; label[length] != '\0' && length < sizeof(clean) - 1u; ++length) {
        unsigned char c = (unsigned char)label[length];

        clean[length] = (isalnum(c) || c == '.' || c == '-' || c == '_')
                            ? (char)c : '_';
    }
    clean[length] = '\0';
    if (length == 0u || strcmp(clean, ".") == 0 || strcmp(clean, "..") == 0) {
        (void)snprintf(clean, sizeof(clean), "camera");
    }
    if (snprintf(out, capacity, "%s/%s.%ld", root, clean, (long)getpid()) >=
        (int)capacity) {
        return false;
    }
    if (mkdir(out, 0700) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

void krtsp_buffer_dir_remove(const char *dir)
{
    DIR *handle;
    struct dirent *entry;

    if (dir == NULL || dir[0] == '\0') {
        return;
    }
    handle = opendir(dir);
    if (handle == NULL) {
        return;
    }
    while ((entry = readdir(handle)) != NULL) {
        char path[PATH_MAX];
        struct stat info;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >=
                (int)sizeof(path)) {
            continue;
        }
        /* Regular files only: this is our own scratch, and a symlink or a
         * nested directory in it is not ours to follow. */
        if (lstat(path, &info) == 0 && S_ISREG(info.st_mode)) {
            (void)unlink(path);
        }
    }
    (void)closedir(handle);
    (void)rmdir(dir);
}

size_t krtsp_buffer_sweep(void)
{
    char root[PATH_MAX];
    DIR *handle;
    struct dirent *entry;
    size_t removed = 0u;

    if (!buffer_root(root, sizeof(root))) {
        return 0u;
    }
    handle = opendir(root);
    if (handle == NULL) {
        return 0u;
    }
    while ((entry = readdir(handle)) != NULL) {
        const char *dot = strrchr(entry->d_name, '.');
        char *end = NULL;
        long pid;
        char path[PATH_MAX];

        if (dot == NULL || dot[1] == '\0') {
            continue;
        }
        errno = 0;
        pid = strtol(dot + 1, &end, 10);
        if (errno != 0 || *end != '\0' || pid <= 0) {
            continue;
        }
        /* ESRCH is the only answer that means "gone"; EPERM means it
         * exists and is somebody else's. */
        if (kill((pid_t)pid, 0) == 0 || errno != ESRCH) {
            continue;
        }
        if (snprintf(path, sizeof(path), "%s/%s", root, entry->d_name) <
            (int)sizeof(path)) {
            krtsp_buffer_dir_remove(path);
            ++removed;
        }
    }
    (void)closedir(handle);
    return removed;
}
