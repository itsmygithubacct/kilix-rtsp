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
 * as the error it is.  Component-owned directories are also a credential
 * boundary: never accept a symlink, a different owner, or public mode bits. */
static bool ensure_dir(const char *path, bool private_directory)
{
    struct stat info;
    bool created = false;

    if (mkdir(path, 0700) == 0) {
        created = true;
        /* mkdir modes are filtered through umask.  These directories are
         * an exact credential boundary, so normalize paths we just created
         * before validating them below. */
        if (chmod(path, 0700) != 0) {
            return false;
        }
        if (!private_directory) {
            return true;
        }
    } else {
        if (errno != EEXIST) {
            return false;
        }
    }
    if ((private_directory || created ? lstat(path, &info) : stat(path, &info))
        != 0) {
        return false;
    }
    if (!S_ISDIR(info.st_mode)) {
        errno = ENOTDIR;
        return false;
    }
    if (private_directory &&
        (info.st_uid != geteuid() || (info.st_mode & 0777) != 0700)) {
        errno = EACCES;
        return false;
    }
    return true;
}

static bool valid_leaf(const char *leaf)
{
    if (leaf == NULL || leaf[0] == '\0') {
        return false;
    }
    for (const unsigned char *scan = (const unsigned char *)leaf;
         *scan != '\0'; ++scan) {
        if (!((*scan >= 'a' && *scan <= 'z') ||
              (*scan >= 'A' && *scan <= 'Z') ||
              (*scan >= '0' && *scan <= '9') || *scan == '-' ||
              *scan == '_')) {
            return false;
        }
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
            !ensure_dir(root, false)) {
            return false;
        }
        printed = snprintf(root, sizeof(root), "%s/.local/gpu_terminal", home);
        if (printed < 0 || (size_t)printed >= sizeof(root) ||
            !ensure_dir(root, false)) {
            return false;
        }
        printed = snprintf(root, sizeof(root),
                           "%s/.local/gpu_terminal/kilix-rtsp", home);
    }
    if (printed < 0 || (size_t)printed >= sizeof(root)) {
        errno = ENAMETOOLONG;
        return false;
    }
    if (!ensure_dir(root, true)) {
        return false;
    }

    if (leaf == NULL || leaf[0] == '\0') {
        printed = snprintf(out, capacity, "%s", root);
    } else {
        /* A leaf is a fixed name from this library, never user input, so
         * it needs no traversal checking - but reject the obvious anyway
         * rather than relying on that staying true. */
        if (!valid_leaf(leaf)) {
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
    if (!ensure_dir(out, true)) {
        out[0] = '\0';
        return false;
    }
    return true;
}
