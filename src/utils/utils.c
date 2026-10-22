/* src/utils/utils.c - Common utility helpers (shared across modules)
 *
 * Directory/file: mkdir_p()       recursive mkdir
 * String/memory:  future extensions go here
 *
 * Depends only on libc + nsbc_common.h.
 * No back-refs to any src/ business subdirectory. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "utils.h"
#include "nsbc_common.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- mkdir -p: recursively create directory ---- */
int mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    char *p;
    size_t len;
    struct stat st;

    if (!path || !*path)
        return 0;

    len = strlen(path);
    if (len >= sizeof(tmp))
        return NSBC_ERR;
    memcpy(tmp, path, len + 1);

    /* Strip trailing filename, keep directory part */
    p = strrchr(tmp, '/');
    if (!p)
        return 0;  /* no directory prefix, current dir is fine */
    *p = '\0';

    /* Segment-by-segment create: start from root, try mkdir at each '/' */
    for (p = tmp + 1; *p; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (stat(tmp, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                errno = ENOTDIR;
                return NSBC_ERR;
            }
        } else if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
            return NSBC_ERR;
        }
        *p = '/';
    }
    /* Final segment */
    if (stat(tmp, &st) != 0) {
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
            return NSBC_ERR;
    } else if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return NSBC_ERR;
    }
    return 0;
}
