// Copyright 2021  Rinat Ibragimov
// SPDX-License-Identifier: MIT

#define _GNU_SOURCE
#include "encfs_mapper.h"
#include "intercepted_functions.h"
#include "log.h"
#include "mem.h"
#include "ut_misc.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#include <utarray.h>
#include <uthash.h>
#include <utstring.h>

#define FUSE_SUPER_MAGIC 0x65735546

struct linux_dirent64 {
    ino64_t d_ino;
    off64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

struct inode_to_path_mapping {
    UT_hash_handle hh;
    uint64_t inode;
    char *path;
};

struct front_to_back_mapping {
    UT_hash_handle hh;
    char *encfs_front;
    char *encfs_back;
    uint64_t encfs_pid;
    bool pending_removal;
};

static struct front_to_back_mapping *front_to_back_map = NULL;
static struct inode_to_path_mapping *inode_to_path_map = NULL;

static UT_icd uint64_icd = {sizeof(uint64_t), NULL, NULL, NULL};

static int
file_get_contents(const char *file_name, UT_string *body)
{
    char buf[4096];
    int fd = real_open(file_name, O_RDONLY);
    if (fd == -1)
        goto err_1;

    utstring_clear(body);

    size_t pos = 0;
    while (1) {
        ssize_t bytes_read = pread(fd, buf, sizeof(buf), pos);
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            goto err_2;
        }
        if (bytes_read == 0) {
            // Sudden file size change?
            break;
        }

        utstring_bincpy(body, buf, bytes_read);
        pos += bytes_read;
    }

    close(fd);
    return 0;

err_2:
    utstring_clear(body);
    close(fd);
err_1:
    return -1;
}

static char *
strdup_and_trim_slashes(const char *source)
{
    char *s = xstrdup(source);

    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == '/')
        s[len - 1] = '\0';
    return s;
}

static void
free_front_to_back_mapping(struct front_to_back_mapping *m)
{
    free(m->encfs_front);
    free(m->encfs_back);
    free(m);
}

static void
clear_front_to_back_map(void)
{
    struct front_to_back_mapping *it, *tmp;

    HASH_ITER (hh, front_to_back_map, it, tmp) {
        HASH_DEL(front_to_back_map, it);
        free_front_to_back_mapping(it);
    }
}

static void
free_inode_to_path_mapping(struct inode_to_path_mapping *m)
{
    free(m->path);
    free(m);
}

static void
clear_inode_to_path_map(void)
{
    struct inode_to_path_mapping *it, *tmp;
    HASH_ITER (hh, inode_to_path_map, it, tmp) {
        HASH_DEL(inode_to_path_map, it);
        free_inode_to_path_mapping(it);
    }
}

static void
remove_inode_to_path_mappings_for_path(const char *back_path)
{
    size_t back_path_len = strlen(back_path);
    struct inode_to_path_mapping *it, *tmp;
    HASH_ITER (hh, inode_to_path_map, it, tmp) {
        size_t it_path_len = strlen(it->path);
        bool it_is_inside_back_path =
            it_path_len >= back_path_len &&
            strncmp(it->path, back_path, back_path_len) == 0 &&
            (it->path[back_path_len] == '/' || it->path[back_path_len] == '\0');

        if (it_is_inside_back_path) {
            HASH_DEL(inode_to_path_map, it);
            free_inode_to_path_mapping(it);
        }
    }
}

static void
process_encfs_mount(UT_string *cmdline, uint64_t pid)
{
    // "/proc/$pid/cmdline" contains command line arguments
    // separated by NUL characters. strlen() is used to find
    // them.
    char *ptr = utstring_body(cmdline);
    char *end = ptr + utstring_len(cmdline);
    char *dirs[2] = {};
    int idx = 0;

    ptr += strlen(ptr) + 1;  // Skip first argument.

    while (ptr < end) {
        if (idx < 2 && ptr[0] != '-')
            dirs[idx++] = ptr;
        ptr = ptr + strlen(ptr) + 1;
    }

    if (dirs[0] == NULL || dirs[1] == NULL) {
        // Encfs should have back and front directories specified in the command
        // line. If they absent, no further processing is possible.
        goto done_1;
    }

    char *encfs_back = strdup_and_trim_slashes(dirs[0]);
    char *encfs_front = strdup_and_trim_slashes(dirs[1]);

    struct front_to_back_mapping *m;
    HASH_FIND_STR(front_to_back_map, encfs_front, m);
    if (m) {
        if (m->encfs_pid == pid) {
            // Leave as is, this is probably the same mount.
            m->pending_removal = false;
            goto done_2;
        }

        // This was other mount.
        remove_inode_to_path_mappings_for_path(m->encfs_back);
        HASH_DEL(front_to_back_map, m);
        free_front_to_back_mapping(m);
    }

    m = xmalloc(sizeof(*m));

    m->encfs_pid = pid;
    m->pending_removal = false;
    m->encfs_back = encfs_back;
    m->encfs_front = encfs_front;
    encfs_back = NULL;
    encfs_front = NULL;

    HASH_ADD_KEYPTR(hh, front_to_back_map, m->encfs_front,
                    strlen(m->encfs_front), m);

done_2:
    free(encfs_back);
    free(encfs_front);
done_1:
    return;
}

static int
do_refresh_mounts(void)
{
    char buf[32 * 1024];
    UT_string fname;
    UT_string cmdline;

    utstring_init(&fname);
    utstring_init(&cmdline);

    for (struct front_to_back_mapping *it = front_to_back_map; it != NULL;
         it = it->hh.next)  //
    {
        it->pending_removal = true;
    }

    int dir_fd = real_open("/proc", O_RDONLY | O_DIRECTORY);
    if (dir_fd == -1)
        goto err;

    while (1) {
        int nread = syscall(SYS_getdents64, dir_fd, buf, sizeof(buf));

        if (nread == -1) {
            close(dir_fd);
            goto err;
        }

        if (nread == 0)
            break;

        int pos = 0;
        while (pos < nread) {
            struct linux_dirent64 *de = (struct linux_dirent64 *)(buf + pos);

            do {
                if (de->d_type != DT_DIR || !isdigit(de->d_name[0]))
                    break;

                utstring_clear(&fname);
                utstring_printf(&fname, "/proc/%s/cmdline", de->d_name);

                int r = file_get_contents(utstring_body(&fname), &cmdline);
                if (r != 0)
                    break;

                if (strncmp(utstring_body(&cmdline), "encfs",
                            sizeof("encfs") - 1 + 1) == 0)  //
                {
                    process_encfs_mount(&cmdline, atol(de->d_name));
                }
            } while (0);

            pos += de->d_reclen;
        }
    }

    {
        struct front_to_back_mapping *it, *tmp;
        HASH_ITER (hh, front_to_back_map, it, tmp) {
            if (it->pending_removal) {
                remove_inode_to_path_mappings_for_path(it->encfs_back);
                HASH_DEL(front_to_back_map, it);
                free_front_to_back_mapping(it);
            }
        }
    }

    close(dir_fd);
    utstring_done(&fname);
    utstring_done(&cmdline);
    return 0;

err:
    utstring_done(&fname);
    utstring_done(&cmdline);
    return -1;
}

int
encfs_mapper_force_refresh_mounts(void)
{
    return do_refresh_mounts();
}

int
encfs_mapper_refresh_mounts(const char *current_path)
{
    static struct timespec last_checked = {};
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return -1;

    if (now.tv_sec == last_checked.tv_sec) {
        // Limits refresh rate to approximately one per second.
        return 0;
    }

    last_checked = now;

    struct statfs sfs;
    int res = statfs(current_path, &sfs);
    if (res != 0) {
        // 'current_path' is expected to be a valid path.
        return -1;
    }

    if (sfs.f_type != FUSE_SUPER_MAGIC) {
        // Can skip update procedure if the path is not even a FUSE mount.
        return 0;
    }

    return do_refresh_mounts();
}

static UT_array *
trace_inodes_back_to_base(const char *src_path, const char *encfs_front)
{
    LOG("%s> src_path=%s, encfs_front=%s", __func__, src_path, encfs_front);
    size_t encfs_front_len = strlen(encfs_front);
    UT_string cur_path;

    utstring_init(&cur_path);
    utstring_bincpy(&cur_path, src_path, strlen(src_path));

    size_t len = utstring_len(&cur_path);
    while (len > 0 && utstring_body(&cur_path)[len - 1] == '/')
        len -= 1;
    utstring_truncate(&cur_path, len);

    UT_array *inode_trace;
    utarray_new(inode_trace, &uint64_icd);

    while (utstring_len(&cur_path) > encfs_front_len) {
        LOG("%s: cur_path=%s", __func__, utstring_body(&cur_path));
        struct stat sb;
        int r = lstat(utstring_body(&cur_path), &sb);
        if (r != 0)
            break;

        uint64_t inode = sb.st_ino;
        utarray_push_back(inode_trace, &inode);
        LOG("%s: inode=%" PRIu64, __func__, inode);

        char *slash =
            memrchr(utstring_body(&cur_path), '/', utstring_len(&cur_path));
        utstring_truncate(&cur_path, slash - utstring_body(&cur_path));
        if (utstring_len(&cur_path) == 0)
            break;
    }

    if (utstring_len(&cur_path) != encfs_front_len) {
        utarray_free(inode_trace);
        utstring_done(&cur_path);
        LOG("%s: error", __func__);
        return NULL;
    }

    utstring_done(&cur_path);
    LOG("%s: returning array of %d elements", __func__,
        utarray_len(inode_trace));
    return inode_trace;
}

static char *
find_inode_in_dir(const char *path, uint64_t target_inode)
{
    LOG("%s> path=%s, target_inode=%" PRIu64, __func__, path, target_inode);
    char buf[32 * 1024];
    char *res = NULL;
    size_t path_len = strlen(path);
    bool path_ends_with_slash = path_len > 0 && path[path_len - 1] == '/';

    int dir_fd = real_open(path, O_RDONLY | O_DIRECTORY);
    if (dir_fd == -1)
        goto err;

    while (1) {
        int nread = syscall(SYS_getdents64, dir_fd, buf, sizeof(buf));

        if (nread == -1 || nread == 0)
            goto done;

        int pos = 0;
        while (pos < nread) {
            struct linux_dirent64 *de = (struct linux_dirent64 *)(buf + pos);
            uint64_t inode = de->d_ino;

            LOG("%s: inode=%" PRIu64, __func__, inode);
            if (inode == target_inode) {
                LOG("%s: inode matches target_inode", __func__);
                UT_string tmp;

                utstring_init(&tmp);
                utstring_printf(&tmp, "%s%s%s", path,
                                path_ends_with_slash ? "" : "/", de->d_name);
                res = utstring_steal_data(&tmp);
                // Continue to enumerate files. Mapping from their inodes to
                // their names will be useful in subsequent searches.
            }

            // Cache inode-to-path mapping.
            LOG("%s: caching inode=%" PRIu64, __func__, inode);
            struct inode_to_path_mapping *m;
            HASH_FIND_UINT64(inode_to_path_map, &inode, m);
            if (!m) {
                UT_string tmp;

                utstring_init(&tmp);
                utstring_printf(&tmp, "%s%s%s", path,
                                path_ends_with_slash ? "" : "/", de->d_name);

                m = xmalloc(sizeof(*m));
                m->inode = inode;
                m->path = utstring_steal_data(&tmp);
                HASH_ADD_UINT64(inode_to_path_map, inode, m);
            }

            pos += de->d_reclen;
        }
    }

done:
    close(dir_fd);
err:
    LOG("%s: returning res=%s", __func__, res);
    return res;
}

static char *
query_inode_to_path_map(uint64_t inode)
{
    struct inode_to_path_mapping *m;
    HASH_FIND_UINT64(inode_to_path_map, &inode, m);
    return m ? xstrdup(m->path) : NULL;
}

static char *
follow_inode_trace(UT_array *inode_trace, const char *base)
{
    LOG("%s> utarray_len(inode_trace)=%d, base=%s", __func__,
        utarray_len(inode_trace), base);
    char *cur_path = NULL;
    ssize_t idx = 0;

    // Try cache first.
    LOG("%s: testing cache", __func__);
    for (/* void */; idx < utarray_len(inode_trace); idx++) {
        struct inode_to_path_mapping *m;
        uint64_t inode = utarray_index(inode_trace, uint64_t, idx);

        LOG("%s: searching for inode=%" PRIu64, __func__, inode);
        HASH_FIND_UINT64(inode_to_path_map, &inode, m);
        if (m) {
            LOG("%s: found inode=%" PRIu64 " with path=%s", __func__, inode,
                m->path);
            cur_path = xstrdup(m->path);
            break;
        }
    }

    LOG("%s: done with cache search, idx=%" PRIi64, __func__, idx);

    // When it's a cache miss, idx is equal to the length of inode_trace, so it
    // needs to be decremented. When it's a cache miss, lookup should start from
    // a next trace point. Since trace is stored backwards, next point is at the
    // previous index. If the hit is at index 0, which is exactly a target file
    // path, no further lookup is necessary, and decrementing idx to -1 skips
    // the lookup cycle.
    idx -= 1;

    if (!cur_path)
        cur_path = xstrdup(base);
    LOG("%s: cur_path=%s, idx=%" PRIi64, __func__, cur_path, idx);

    for (/* void */; idx >= 0; idx--) {
        uint64_t inode = utarray_index(inode_trace, uint64_t, idx);
        LOG("%s: searching inode=%" PRIu64 " at cur_path=%s", __func__, inode,
            cur_path);
        char *new_path = find_inode_in_dir(cur_path, inode);
        LOG("%s: found path %s", __func__, new_path);

        free(cur_path);
        cur_path = new_path;
        if (!new_path) {
            cur_path = NULL;
            break;
        }
    }

    LOG("%s: returning cur_path=%s", __func__, cur_path);
    return cur_path;
}

char *
encfs_mapper_resolve_path(const char *src_path)
{
    LOG("%s> src_path=%s", __func__, src_path);
    struct statfs sfsb;
    char *res = NULL;
    size_t src_path_len = strlen(src_path);

    int r = statfs(src_path, &sfsb);
    if (r != 0) {
        res = NULL;
        goto done;
    }

    if (sfsb.f_type != FUSE_SUPER_MAGIC) {
        LOG("%s: not a FUSE mount", __func__);
        res = xstrdup(src_path);
        goto done;
    }

    LOG("%s: probing known encfs mounts", __func__);
    for (struct front_to_back_mapping *it = front_to_back_map; it != NULL;
         it = it->hh.next) {
        LOG("%s: probing it->encfs_front=%s", __func__, it->encfs_front);
        size_t encfs_front_len = strlen(it->encfs_front);
        bool in_encfs =
            src_path_len >= encfs_front_len &&
            strncmp(src_path, it->encfs_front, encfs_front_len) == 0 &&
            (src_path[encfs_front_len] == '/' ||
             src_path[encfs_front_len] == '\0');

        if (!in_encfs)
            continue;

        LOG("%s: found a match", __func__);

        struct stat sb;
        r = lstat(src_path, &sb);
        if (r != 0 || (sb.st_mode & S_IFMT) != S_IFREG) {
            LOG("%s: lstat failed or not a regular file", __func__);
            break;
        }

        res = query_inode_to_path_map(sb.st_ino);
        if (res) {
            LOG("%s: query_inode_to_path_map returned some path", __func__);
            break;
        }

        UT_array *inode_trace =
            trace_inodes_back_to_base(src_path, it->encfs_front);

        res = follow_inode_trace(inode_trace, it->encfs_back);
        utarray_free(inode_trace);
        break;
    }

    if (!res)
        res = xstrdup(src_path);

done:
    LOG("%s: returning res=%s", __func__, res);
    return res;
}

void
encfs_mapper_cleanup(void)
{
    clear_front_to_back_map();
    clear_inode_to_path_map();
}
