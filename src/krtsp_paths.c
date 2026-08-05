/*
 * Where the running program keeps its files.
 *
 * Everything lives under ~/.local/gpu_terminal/kilix-rtsp, matching the
 * sibling projects, and deliberately outside the repository.  That keeps
 * the work tree free of uncommittable files, and it is also a security
 * boundary: camera configuration embeds credentials in RTSP URLs, so the
 * config file is a secret and must never sit where a stray `git add -A`
 * could sweep it up.
 */

#include "kilix_rtsp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* mkdir that treats an existing directory as success but a non-directory
 * as the error it is. */
static bool ensure_dir(const char *path)
{
    struct stat info;

    if (mkdir(path, 0700) == 0) {
        return true;
    }
    if (errno != EEXIST) {
        return false;
    }
    if (stat(path, &info) != 0) {
        return false;
    }
    if (!S_ISDIR(info.st_mode)) {
        errno = ENOTDIR;
        return false;
    }
    return true;
}

bool krtsp_paths_dir(const char *leaf, char *out, size_t capacity)
{
    const char *override;
    const char *home;
    char root[512];
    int printed;

    if (out == NULL || capacity == 0u) {
        return false;
    }
    out[0] = '\0';

    override = getenv("KILIX_RTSP_HOME");
    if (override != NULL && override[0] != '\0') {
        printed = snprintf(root, sizeof(root), "%s", override);
    } else {
        home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            errno = ENOENT;
            return false;
        }
        /* Create the intermediate levels too: a fresh machine has
         * neither ~/.local/gpu_terminal nor our directory under it. */
        printed = snprintf(root, sizeof(root), "%s/.local", home);
        if (printed < 0 || (size_t)printed >= sizeof(root) ||
            !ensure_dir(root)) {
            return false;
        }
        printed = snprintf(root, sizeof(root), "%s/.local/gpu_terminal", home);
        if (printed < 0 || (size_t)printed >= sizeof(root) ||
            !ensure_dir(root)) {
            return false;
        }
        printed = snprintf(root, sizeof(root),
                           "%s/.local/gpu_terminal/kilix-rtsp", home);
    }
    if (printed < 0 || (size_t)printed >= sizeof(root)) {
        errno = ENAMETOOLONG;
        return false;
    }
    if (!ensure_dir(root)) {
        return false;
    }

    if (leaf == NULL || leaf[0] == '\0') {
        printed = snprintf(out, capacity, "%s", root);
    } else {
        /* A leaf is a fixed name from this library, never user input, so
         * it needs no traversal checking - but reject the obvious anyway
         * rather than relying on that staying true. */
        if (strchr(leaf, '/') != NULL || strcmp(leaf, "..") == 0) {
            errno = EINVAL;
            return false;
        }
        printed = snprintf(out, capacity, "%s/%s", root, leaf);
    }
    if (printed < 0 || (size_t)printed >= capacity) {
        out[0] = '\0';
        errno = ENAMETOOLONG;
        return false;
    }
    if (!ensure_dir(out)) {
        out[0] = '\0';
        return false;
    }
    return true;
}
