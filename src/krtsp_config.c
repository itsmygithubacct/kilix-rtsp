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
#include <fcntl.h>
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
static bool identifier_character(unsigned char character)
{
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '-' ||
           character == '_';
}

static bool safe_name(const char *name)
{
    size_t length;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    length = strlen(name);
    if (name[0] == ' ' || name[length - 1u] == ' ') {
        return false;
    }
    for (const unsigned char *scan = (const unsigned char *)name;
         *scan != '\0'; ++scan) {
        if (*scan < 0x20u || *scan == 0x7fu || *scan == ',') {
            return false;
        }
    }
    return true;
}

static bool safe_value(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    for (const unsigned char *scan = (const unsigned char *)value;
         *scan != '\0'; ++scan) {
        if (*scan < 0x20u || *scan == 0x7fu) {
            return false;
        }
    }
    return true;
}

static bool safe_key(const char *key)
{
    if (key == NULL || key[0] == '\0') {
        return false;
    }
    for (const unsigned char *scan = (const unsigned char *)key;
         *scan != '\0'; ++scan) {
        if (!identifier_character(*scan)) {
            return false;
        }
    }
    return true;
}

static bool parse_section(const char *line, char *kind, size_t kind_capacity,
                          char *name, size_t name_capacity)
{
    const char *scan = line;
    const char *kind_start;
    const char *name_start;
    size_t kind_length;
    size_t name_length;

    if (*scan++ != '[') {
        return false;
    }
    while (*scan == ' ' || *scan == '\t') {
        scan++;
    }
    kind_start = scan;
    while (identifier_character((unsigned char)*scan)) {
        scan++;
    }
    kind_length = (size_t)(scan - kind_start);
    if (kind_length == 0u || kind_length >= kind_capacity ||
        (*scan != ' ' && *scan != '\t')) {
        return false;
    }
    while (*scan == ' ' || *scan == '\t') {
        scan++;
    }
    if (*scan++ != '"') {
        return false;
    }
    name_start = scan;
    while (*scan != '\0' && *scan != '"') {
        scan++;
    }
    name_length = (size_t)(scan - name_start);
    if (name_length == 0u || name_length >= name_capacity) {
        return false;
    }
    if (*scan++ != '"') {
        return false;
    }
    while (*scan == ' ' || *scan == '\t') {
        scan++;
    }
    if (*scan++ != ']') {
        return false;
    }
    while (*scan == ' ' || *scan == '\t') {
        scan++;
    }
    if (*scan != '\0') {
        return false;
    }

    memcpy(kind, kind_start, kind_length);
    kind[kind_length] = '\0';
    memcpy(name, name_start, name_length);
    name[name_length] = '\0';
    if (!safe_name(name)) {
        return false;
    }
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

/* Open first and validate that exact descriptor.  A stat(path) followed by
 * fopen(path) lets a rename swap in a different file between the checks. */
static FILE *open_credential_file(
    const char *path, char *error, size_t error_capacity)
{
    struct stat info;
    FILE *handle;
    int status_flags;
    int fd;

    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ELOOP) {
            set_error(error, error_capacity,
                      "%s is a symbolic link; use the credential file itself",
                      path);
        } else {
            set_error(error, error_capacity, "cannot open %s: %s", path,
                      strerror(errno));
        }
        return NULL;
    }
    if (fstat(fd, &info) != 0) {
        set_error(error, error_capacity, "cannot inspect %s: %s", path,
                  strerror(errno));
        (void)close(fd);
        return NULL;
    }
    if (!S_ISREG(info.st_mode)) {
        set_error(error, error_capacity, "%s is not a regular file", path);
        (void)close(fd);
        return NULL;
    }
    if (info.st_uid != geteuid()) {
        set_error(error, error_capacity,
                  "%s is owned by uid %ld, not by this user", path,
                  (long)info.st_uid);
        (void)close(fd);
        return NULL;
    }
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        set_error(error, error_capacity,
                  "%s is readable by others (mode %04o); it holds camera "
                  "passwords - run: chmod 600 %s",
                  path, (unsigned)(info.st_mode & 07777), path);
        (void)close(fd);
        return NULL;
    }
    status_flags = fcntl(fd, F_GETFL);
    if (status_flags < 0 ||
        fcntl(fd, F_SETFL, status_flags & ~O_NONBLOCK) != 0) {
        set_error(error, error_capacity, "cannot prepare %s: %s", path,
                  strerror(errno));
        (void)close(fd);
        return NULL;
    }
    handle = fdopen(fd, "r");
    if (handle == NULL) {
        set_error(error, error_capacity, "cannot read %s: %s", path,
                  strerror(errno));
        (void)close(fd);
        return NULL;
    }
    return handle;
}

typedef enum config_line_result {
    CONFIG_LINE_END = 0,
    CONFIG_LINE_OK,
    CONFIG_LINE_TOO_LONG,
    CONFIG_LINE_BINARY,
    CONFIG_LINE_IO_ERROR
} config_line_result;

static config_line_result read_config_line(FILE *handle, char *line,
                                           size_t capacity)
{
    size_t used = 0u;

    for (;;) {
        int character = fgetc(handle);

        if (character == EOF) {
            if (ferror(handle)) {
                return CONFIG_LINE_IO_ERROR;
            }
            if (used == 0u) {
                return CONFIG_LINE_END;
            }
            line[used] = '\0';
            return CONFIG_LINE_OK;
        }
        if (character == 0) {
            return CONFIG_LINE_BINARY;
        }
        if (character == '\n') {
            line[used] = '\0';
            return CONFIG_LINE_OK;
        }
        if (used + 1u >= capacity) {
            return CONFIG_LINE_TOO_LONG;
        }
        line[used++] = (char)character;
    }
}

static bool add_group_members(krtsp_group *group, char *value,
                              int line_number, char *error,
                              size_t error_capacity)
{
    char *cursor = value;

    for (;;) {
        char *comma = strchr(cursor, ',');
        char *member;

        if (comma != NULL) {
            *comma = '\0';
        }
        member = trim(cursor);
        if (!safe_name(member)) {
            set_error(error, error_capacity,
                      "line %d: empty or invalid camera name in group '%s'",
                      line_number, group->name);
            return false;
        }
        for (size_t index = 0u; index < group->member_count; ++index) {
            if (strcmp(group->members[index], member) == 0) {
                set_error(error, error_capacity,
                          "line %d: duplicate camera '%s' in group '%s'",
                          line_number, member, group->name);
                return false;
            }
        }
        if (group->member_count >= KRTSP_GROUP_MEMBERS_MAX) {
            set_error(error, error_capacity,
                      "line %d: too many cameras in group '%s'", line_number,
                      group->name);
            return false;
        }
        if (!copy_field(group->members[group->member_count], KRTSP_NAME_MAX,
                        member)) {
            set_error(error, error_capacity,
                      "line %d: camera name too long", line_number);
            return false;
        }
        group->member_count++;
        if (comma == NULL) {
            return true;
        }
        cursor = comma + 1;
    }
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
        int printed = snprintf(resolved, sizeof(resolved), "%s/cameras.conf",
                               directory);

        if (printed < 0 || (size_t)printed >= sizeof(resolved)) {
            set_error(error, error_capacity,
                      "configuration path is too long");
            return false;
        }
        path = resolved;
    }

    handle = open_credential_file(path, error, error_capacity);
    if (handle == NULL) {
        return false;
    }
    config = calloc(1u, sizeof(*config));
    if (config == NULL) {
        (void)fclose(handle);
        set_error(error, error_capacity, "out of memory");
        return false;
    }

    for (;;) {
        char *text;
        char *key;
        char *value;
        config_line_result line_result =
            read_config_line(handle, line, sizeof(line));

        if (line_result == CONFIG_LINE_END) {
            break;
        }

        line_number++;
        if (line_result != CONFIG_LINE_OK) {
            if (line_result == CONFIG_LINE_TOO_LONG) {
                set_error(error, error_capacity,
                          "line %d: longer than %zu bytes", line_number,
                          sizeof(line) - 1u);
            } else if (line_result == CONFIG_LINE_BINARY) {
                set_error(error, error_capacity,
                          "line %d: configuration contains a NUL byte",
                          line_number);
            } else {
                set_error(error, error_capacity,
                          "line %d: error reading configuration: %s",
                          line_number, strerror(errno));
            }
            ok = false;
            break;
        }
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
                if (krtsp_config_find(config, name) != NULL ||
                    krtsp_config_find_group(config, name) != NULL) {
                    set_error(error, error_capacity,
                              "line %d: duplicate section name '%s'",
                              line_number, name);
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
                if (krtsp_config_find(config, name) != NULL ||
                    krtsp_config_find_group(config, name) != NULL) {
                    set_error(error, error_capacity,
                              "line %d: duplicate section name '%s'",
                              line_number, name);
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
        if (!safe_key(key)) {
            set_error(error, error_capacity,
                      "line %d: invalid key", line_number);
            ok = false;
            break;
        }
        if (camera != NULL) {
            /* Never echo `value`: it is a URL, and a URL is a password. */
            if (strcmp(key, "main") == 0) {
                if (!safe_value(value)) {
                    set_error(error, error_capacity,
                              "line %d: main url is empty or contains a "
                              "control character", line_number);
                    ok = false;
                    break;
                }
                if (camera->url_main[0] != '\0') {
                    set_error(error, error_capacity,
                              "line %d: duplicate main url", line_number);
                    ok = false;
                    break;
                }
                if (!copy_field(camera->url_main, sizeof(camera->url_main),
                                value)) {
                    set_error(error, error_capacity,
                              "line %d: main url longer than %d bytes",
                              line_number, KRTSP_URL_MAX - 1);
                    ok = false;
                    break;
                }
            } else if (strcmp(key, "sub") == 0) {
                if (!safe_value(value)) {
                    set_error(error, error_capacity,
                              "line %d: sub url is empty or contains a "
                              "control character", line_number);
                    ok = false;
                    break;
                }
                if (camera->url_sub[0] != '\0') {
                    set_error(error, error_capacity,
                              "line %d: duplicate sub url", line_number);
                    ok = false;
                    break;
                }
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
            if (!add_group_members(group, value, line_number, error,
                                   error_capacity)) {
                ok = false;
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
    if (ok) {
        for (size_t index = 0u; index < config->group_count; ++index) {
            const krtsp_group *entry = &config->groups[index];

            if (entry->member_count == 0u) {
                set_error(error, error_capacity,
                          "group '%s' has no cameras", entry->name);
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
    if (camera == NULL ||
        (tier != KRTSP_TIER_MAIN && tier != KRTSP_TIER_SUB)) {
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
