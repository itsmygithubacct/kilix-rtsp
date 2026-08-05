/*
 * Camera definitions.
 *
 * The format is the one in examples/cameras.conf.example: sections
 * introduced by [camera "name"] or [group "name"], and key = value lines
 * inside them.
 *
 * Two rules here are not stylistic:
 *
 * 1. A tier's URL is never synthesized from another tier's.  Three
 *    incompatible path conventions are in use on the reference fleet
 *    (/stream1|2, /live/ch0|ch1, /tcp/av0_N) and one camera is on a
 *    non-standard port, so a "just swap the last digit" rule would be
 *    wrong for most of them and silently wrong for the rest.
 *
 * 2. The file is refused unless it is a regular file, owned by the
 *    caller, with no group or world permission bits.  It contains
 *    passwords.  Warning and continuing would defeat the reason it lives
 *    outside the repository in the first place.
 *
 * Errors never quote a URL.  A parse error that helpfully echoes the
 * offending line would put a password in a log.
 */

#include "kilix_rtsp.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct krtsp_config {
    krtsp_camera cameras[KRTSP_CAMERAS_MAX];
    size_t camera_count;
    krtsp_group groups[KRTSP_CAMERAS_MAX];
    size_t group_count;
};

static void set_error(char *error, size_t capacity, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static void set_error(char *error, size_t capacity, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || capacity == 0u) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(error, capacity, format, arguments);
    va_end(arguments);
}

static char *trim(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t') {
        text++;
    }
    end = text + strlen(text);
    while (end > text) {
        char previous = end[-1];

        if (previous == ' ' || previous == '\t' ||
            previous == '\r' || previous == '\n') {
            end--;
        } else {
            break;
        }
    }
    *end = '\0';
    return text;
}

/* Parse [kind "name"] into kind and name.  Returns false when the line is
 * not a section header of that shape. */
static bool parse_section(char *line, char *kind, size_t kind_capacity,
                          char *name, size_t name_capacity)
{
    char *close_bracket;
    char *first_quote;
    char *last_quote;
    size_t kind_length;
    size_t name_length;

    if (line[0] != '[') {
        return false;
    }
    close_bracket = strrchr(line, ']');
    if (close_bracket == NULL) {
        return false;
    }
    *close_bracket = '\0';
    first_quote = strchr(line + 1, '"');
    if (first_quote == NULL) {
        return false;
    }
    last_quote = strrchr(first_quote + 1, '"');
    if (last_quote == NULL || last_quote == first_quote) {
        return false;
    }
    *last_quote = '\0';

    {
        char *kind_text = line + 1;
        char *kind_end = first_quote;

        while (kind_end > kind_text &&
               (kind_end[-1] == ' ' || kind_end[-1] == '\t')) {
            kind_end--;
        }
        *kind_end = '\0';
        kind_text = trim(kind_text);
        kind_length = strlen(kind_text);
        if (kind_length == 0u || kind_length >= kind_capacity) {
            return false;
        }
        memcpy(kind, kind_text, kind_length + 1u);
    }
    name_length = strlen(first_quote + 1);
    if (name_length == 0u || name_length >= name_capacity) {
        return false;
    }
    memcpy(name, first_quote + 1, name_length + 1u);
    return true;
}

/* Split "key = value" on the FIRST '=': a URL routinely contains '=' in a
 * query string, so splitting on the last one would truncate it. */
static bool parse_pair(char *line, char **key, char **value)
{
    char *equals = strchr(line, '=');

    if (equals == NULL) {
        return false;
    }
    *equals = '\0';
    *key = trim(line);
    *value = trim(equals + 1);
    return (*key)[0] != '\0';
}

static bool copy_field(char *destination, size_t capacity, const char *source)
{
    size_t length = strlen(source);

    if (length >= capacity) {
        return false;
    }
    memcpy(destination, source, length + 1u);
    return true;
}

/* Check ownership and permissions before reading a file of passwords. */
static bool credential_file_is_safe(
    const char *path, char *error, size_t error_capacity)
{
    struct stat info;

    if (stat(path, &info) != 0) {
        set_error(error, error_capacity, "cannot stat %s: %s", path,
                  strerror(errno));
        return false;
    }
    if (!S_ISREG(info.st_mode)) {
        set_error(error, error_capacity, "%s is not a regular file", path);
        return false;
    }
    if (info.st_uid != geteuid()) {
        set_error(error, error_capacity,
                  "%s is owned by uid %ld, not by this user", path,
                  (long)info.st_uid);
        return false;
    }
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        set_error(error, error_capacity,
                  "%s is readable by others (mode %04o); it holds camera "
                  "passwords - run: chmod 600 %s",
                  path, (unsigned)(info.st_mode & 07777), path);
        return false;
    }
    return true;
}

bool krtsp_config_load(
    krtsp_config **out, const char *path, char *error, size_t error_capacity)
{
    char resolved[512];
    char line[1024];
    char kind[32];
    char name[KRTSP_NAME_MAX];
    krtsp_config *config;
    FILE *handle;
    krtsp_camera *camera = NULL;
    krtsp_group *group = NULL;
    int line_number = 0;
    bool ok = true;

    if (error != NULL && error_capacity > 0u) {
        error[0] = '\0';
    }
    if (out == NULL) {
        return false;
    }
    *out = NULL;

    if (path == NULL || path[0] == '\0') {
        char directory[512];

        if (!krtsp_paths_dir("config", directory, sizeof(directory))) {
            set_error(error, error_capacity,
                      "cannot resolve the configuration directory: %s",
                      strerror(errno));
            return false;
        }
        if (snprintf(resolved, sizeof(resolved), "%s/cameras.conf",
                     directory) < 0) {
            return false;
        }
        path = resolved;
    }

    if (!credential_file_is_safe(path, error, error_capacity)) {
        return false;
    }
    handle = fopen(path, "r");
    if (handle == NULL) {
        set_error(error, error_capacity, "cannot open %s: %s", path,
                  strerror(errno));
        return false;
    }
    config = calloc(1u, sizeof(*config));
    if (config == NULL) {
        (void)fclose(handle);
        set_error(error, error_capacity, "out of memory");
        return false;
    }

    while (fgets(line, sizeof(line), handle) != NULL) {
        char *text;
        char *key;
        char *value;

        line_number++;
        text = trim(line);
        if (text[0] == '\0' || text[0] == '#' || text[0] == ';') {
            continue;
        }

        if (text[0] == '[') {
            if (!parse_section(text, kind, sizeof(kind), name,
                               sizeof(name))) {
                set_error(error, error_capacity,
                          "line %d: expected [camera \"name\"] or "
                          "[group \"name\"]", line_number);
                ok = false;
                break;
            }
            camera = NULL;
            group = NULL;
            if (strcmp(kind, "camera") == 0) {
                if (config->camera_count >= KRTSP_CAMERAS_MAX) {
                    set_error(error, error_capacity,
                              "line %d: more than %d cameras", line_number,
                              KRTSP_CAMERAS_MAX);
                    ok = false;
                    break;
                }
                if (krtsp_config_find(config, name) != NULL) {
                    set_error(error, error_capacity,
                              "line %d: duplicate camera '%s'", line_number,
                              name);
                    ok = false;
                    break;
                }
                camera = &config->cameras[config->camera_count++];
                if (!copy_field(camera->name, sizeof(camera->name), name)) {
                    set_error(error, error_capacity,
                              "line %d: camera name too long", line_number);
                    ok = false;
                    break;
                }
            } else if (strcmp(kind, "group") == 0) {
                if (config->group_count >= KRTSP_CAMERAS_MAX) {
                    set_error(error, error_capacity,
                              "line %d: too many groups", line_number);
                    ok = false;
                    break;
                }
                group = &config->groups[config->group_count++];
                if (!copy_field(group->name, sizeof(group->name), name)) {
                    set_error(error, error_capacity,
                              "line %d: group name too long", line_number);
                    ok = false;
                    break;
                }
            } else {
                set_error(error, error_capacity,
                          "line %d: unknown section type '%s'", line_number,
                          kind);
                ok = false;
                break;
            }
            continue;
        }

        if (!parse_pair(text, &key, &value)) {
            set_error(error, error_capacity,
                      "line %d: expected 'key = value'", line_number);
            ok = false;
            break;
        }
        if (camera != NULL) {
            /* Never echo `value`: it is a URL, and a URL is a password. */
            if (strcmp(key, "main") == 0) {
                if (!copy_field(camera->url_main, sizeof(camera->url_main),
                                value)) {
                    set_error(error, error_capacity,
                              "line %d: main url longer than %d bytes",
                              line_number, KRTSP_URL_MAX - 1);
                    ok = false;
                    break;
                }
            } else if (strcmp(key, "sub") == 0) {
                if (!copy_field(camera->url_sub, sizeof(camera->url_sub),
                                value)) {
                    set_error(error, error_capacity,
                              "line %d: sub url longer than %d bytes",
                              line_number, KRTSP_URL_MAX - 1);
                    ok = false;
                    break;
                }
            } else {
                set_error(error, error_capacity,
                          "line %d: unknown camera key '%s'", line_number,
                          key);
                ok = false;
                break;
            }
        } else if (group != NULL) {
            if (strcmp(key, "cameras") != 0) {
                set_error(error, error_capacity,
                          "line %d: unknown group key '%s'", line_number, key);
                ok = false;
                break;
            }
            for (char *token = strtok(value, ","); token != NULL;
                 token = strtok(NULL, ",")) {
                char *member = trim(token);

                if (member[0] == '\0') {
                    continue;
                }
                if (group->member_count >= KRTSP_GROUP_MEMBERS_MAX) {
                    set_error(error, error_capacity,
                              "line %d: too many cameras in group '%s'",
                              line_number, group->name);
                    ok = false;
                    break;
                }
                if (!copy_field(group->members[group->member_count],
                                KRTSP_NAME_MAX, member)) {
                    set_error(error, error_capacity,
                              "line %d: camera name too long", line_number);
                    ok = false;
                    break;
                }
                group->member_count++;
            }
            if (!ok) {
                break;
            }
        } else {
            set_error(error, error_capacity,
                      "line %d: '%s' appears before any section", line_number,
                      key);
            ok = false;
            break;
        }
    }
    (void)fclose(handle);

    /* A camera with no URL at all is a typo that would otherwise show up
     * much later as a source that never starts. */
    if (ok) {
        for (size_t index = 0u; index < config->camera_count; ++index) {
            const krtsp_camera *entry = &config->cameras[index];

            if (entry->url_main[0] == '\0' && entry->url_sub[0] == '\0') {
                set_error(error, error_capacity,
                          "camera '%s' has neither a main nor a sub url",
                          entry->name);
                ok = false;
                break;
            }
        }
    }
    /* A group naming a camera that does not exist is likewise better
     * caught here than as an empty tile. */
    if (ok) {
        for (size_t index = 0u; index < config->group_count && ok; ++index) {
            const krtsp_group *entry = &config->groups[index];

            for (size_t member = 0u; member < entry->member_count; ++member) {
                if (krtsp_config_find(config, entry->members[member]) == NULL) {
                    set_error(error, error_capacity,
                              "group '%s' names unknown camera '%s'",
                              entry->name, entry->members[member]);
                    ok = false;
                    break;
                }
            }
        }
    }

    if (!ok) {
        free(config);
        return false;
    }
    *out = config;
    return true;
}

void krtsp_config_free(krtsp_config *config)
{
    free(config);
}

size_t krtsp_config_camera_count(const krtsp_config *config)
{
    return config != NULL ? config->camera_count : 0u;
}

const krtsp_camera *krtsp_config_camera_at(
    const krtsp_config *config, size_t index)
{
    if (config == NULL || index >= config->camera_count) {
        return NULL;
    }
    return &config->cameras[index];
}

const krtsp_camera *krtsp_config_find(
    const krtsp_config *config, const char *name)
{
    if (config == NULL || name == NULL) {
        return NULL;
    }
    for (size_t index = 0u; index < config->camera_count; ++index) {
        if (strcmp(config->cameras[index].name, name) == 0) {
            return &config->cameras[index];
        }
    }
    return NULL;
}

size_t krtsp_config_group_count(const krtsp_config *config)
{
    return config != NULL ? config->group_count : 0u;
}

const krtsp_group *krtsp_config_group_at(
    const krtsp_config *config, size_t index)
{
    if (config == NULL || index >= config->group_count) {
        return NULL;
    }
    return &config->groups[index];
}

const krtsp_group *krtsp_config_find_group(
    const krtsp_config *config, const char *name)
{
    if (config == NULL || name == NULL) {
        return NULL;
    }
    for (size_t index = 0u; index < config->group_count; ++index) {
        if (strcmp(config->groups[index].name, name) == 0) {
            return &config->groups[index];
        }
    }
    return NULL;
}

const char *krtsp_camera_url(const krtsp_camera *camera, krtsp_tier tier)
{
    if (camera == NULL) {
        return NULL;
    }
    if (tier == KRTSP_TIER_MAIN) {
        if (camera->url_main[0] != '\0') {
            return camera->url_main;
        }
        return camera->url_sub[0] != '\0' ? camera->url_sub : NULL;
    }
    if (camera->url_sub[0] != '\0') {
        return camera->url_sub;
    }
    return camera->url_main[0] != '\0' ? camera->url_main : NULL;
}
