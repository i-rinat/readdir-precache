// Copyright 2021  Rinat Ibragimov
// SPDX-License-Identifier: MIT

#define _GNU_SOURCE
#include "encfs_mapper.h"
#include "intercepted_functions.h"
#include "log.h"
#include "mem.h"
#include "ut_misc.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utarray.h>
#include <uthash.h>
#include <utlist.h>
#include <utstring.h>

#if _DIRENT_MATCHES_DIRENT64 == 0
#error "struct dirent64" is expected to precisely match "struct dirent".
#endif

#define PRECACHE_EXPORT                                                        \
    __attribute__((weak)) __attribute__((visibility("default")))

#define get_mode()                                                             \
    ({                                                                         \
        int ____mode = 0;                                                      \
        if (__OPEN_NEEDS_MODE(oflag)) {                                        \
            va_list a;                                                         \
            va_start(a, oflag);                                                \
            ____mode = va_arg(a, int);                                         \
            va_end(a);                                                         \
        }                                                                      \
        ____mode;                                                              \
    })

enum readdir_tracker_state {
    RDT_STATE_start = 0,       // Initial state.
    RDT_STATE_readdir1_open0,  // Seen one readdir.
    RDT_STATE_readdir1_open1,  // Seen one readdir and one open.
    RDT_STATE_readdir2_open1,  // Seen two readdir's and one open.
    RDT_STATE_readdir2_open2,  // Seen two readdir's and two open's.
    RDT_STATE_readdir3_open2,  // Seen three readdir's and two open's.
    RDT_STATE_do_precaching,   // Seen three readdir's and three open's. Final
                               // FSM state. Resolution: do file precaching.
    RDT_STATE_skip,  // Final FSM state. Resolution: do not do precaching.
};

struct dirent_list {
    struct dirent *ent;
    struct dirent_list *prev, *next;
};

struct dirp_to_state_mapping {
    UT_hash_handle hh;
    DIR *dirp;
    char *dirname;
    struct dirent_list *dirent_list;
    struct dirent_list *current_dirent;
    int cached_files_count;
    enum readdir_tracker_state fsm_state;
};

static struct dirp_to_state_mapping *dirp_to_state_map = NULL;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static void
lock(void)
{
    pthread_mutex_lock(&mutex);
}

static void
unlock(void)
{
    pthread_mutex_unlock(&mutex);
}

static void
free_dirp_to_state_mapping(struct dirp_to_state_mapping *m)
{
    free(m->dirname);
    free(m);
}

static void
clear_dirp_to_state_map(void)
{
    struct dirp_to_state_mapping *it, *tmp;
    HASH_ITER (hh, dirp_to_state_map, it, tmp) {
        HASH_DEL(dirp_to_state_map, it);
        free_dirp_to_state_mapping(it);
    }
}

static void
drain_dirent_list(struct dirp_to_state_mapping *dstate)
{
    while (dstate->dirent_list) {
        struct dirent_list *li = dstate->dirent_list;

        DL_DELETE(dstate->dirent_list, li);
        free(li->ent);
        free(li);
    }
}

static void
populate_dirent_list(struct dirp_to_state_mapping *dstate)
{
    struct dirent *de;

    while ((de = real_readdir(dstate->dirp))) {
        void *mem = xmalloc(de->d_reclen);
        struct dirent_list *li = xmalloc(sizeof(*li));

        memcpy(mem, de, de->d_reclen);
        li->ent = mem;
        DL_APPEND(dstate->dirent_list, li);
    }

    dstate->current_dirent = dstate->dirent_list;
}

static void
handle_opendir(const char *dirname, DIR *dirp)
{
    struct dirp_to_state_mapping *dstate = NULL;

    if (!dirp)
        return;

    lock();
    encfs_mapper_refresh_mounts(dirname);

    HASH_FIND_PTR(dirp_to_state_map, &dirp, dstate);
    if (dstate) {
        // TODO: this is an error state. Hashtable should have no records for
        // this particular 'dirp'.
        HASH_DEL(dirp_to_state_map, dstate);
        free_dirp_to_state_mapping(dstate);
    }

    dstate = xcalloc(1, sizeof(*dstate));

    dstate->dirp = dirp;
    dstate->dirent_list = NULL;
    dstate->current_dirent = NULL;
    dstate->cached_files_count = 0;
    dstate->fsm_state = RDT_STATE_start;
    dstate->dirname = xstrdup(dirname);

    populate_dirent_list(dstate);

    HASH_ADD_PTR(dirp_to_state_map, dirp, dstate);

    unlock();
}

PRECACHE_EXPORT
DIR *
opendir(const char *name)
{
    ensure_initialized();
    LOG("%s: name=%s", __func__, name);

    DIR *dirp = real_opendir(name);
    handle_opendir(name, dirp);
    return dirp;
}

struct sort_array_entry {
    char *file_name;
    uint64_t physical_pos;
    uint64_t file_offset;
    uint64_t extent_length;
};

static int
sort_array_comparator(const void *a, const void *b)
{
    const struct sort_array_entry *a_ = a;
    const struct sort_array_entry *b_ = b;

    return (a_->physical_pos < b_->physical_pos)
               ? -1
               : (a_->physical_pos > b_->physical_pos);
}

static void
sort_array_entry_dtor(void *a)
{
    struct sort_array_entry *a_ = a;
    free(a_->file_name);
}

static void
cache_files(struct dirp_to_state_mapping *dstate)
{
    LOG("%s>", __func__);

    bool cfg_call_sync = true;
    const char *env_PRECACHE_SYNC = getenv("PRECACHE_SYNC");
    if (env_PRECACHE_SYNC)
        cfg_call_sync = atol(env_PRECACHE_SYNC) != 0;

    size_t cfg_cache_limit = (size_t)1 * 1024 * 1024 * 1024;
    const char *env_PRECACHE_LIMIT = getenv("PRECACHE_LIMIT");
    if (env_PRECACHE_LIMIT)
        cfg_cache_limit = atol(env_PRECACHE_LIMIT);

    if (cfg_call_sync)
        system("sync");

    size_t size_so_far = 0;
    size_t count = 0;
    UT_string fname;
    uint32_t extent_buffer_elements = 1000;
    struct fiemap *fiemap = NULL;
    UT_array sort_array;
    UT_icd sort_array_icd = {sizeof(struct sort_array_entry), NULL, NULL,
                             sort_array_entry_dtor};

    utstring_init(&fname);
    utarray_init(&sort_array, &sort_array_icd);

    // Valgrind currently doesn't know about FIEMAP ioctls.
    fiemap = xcalloc(1, sizeof(struct fiemap) + sizeof(struct fiemap_extent) *
                                                    extent_buffer_elements);

    LOG("%s: preparing file list", __func__);
    for (struct dirent_list *it = dstate->current_dirent; it != NULL;
         it = it->next, count++)  //
    {
        struct stat sb;

        if (strcmp(it->ent->d_name, ".") == 0 ||
            strcmp(it->ent->d_name, "..") == 0)  //
        {
            continue;
        }

        utstring_clear(&fname);
        utstring_printf(&fname, "%s/%s", dstate->dirname, it->ent->d_name);
        char *resolved_path = encfs_mapper_resolve_path(utstring_body(&fname));
        LOG("%s: unsorted, path=%s", __func__, utstring_body(&fname));
        LOG("%s: unsorted, resolved-path=%s", __func__, resolved_path);
        if (!resolved_path)
            break;

        int fd = real_open(resolved_path, O_RDONLY);
        if (fd < 0) {
            free(resolved_path);
            continue;
        }

        int res = fstat(fd, &sb);
        if (res != 0) {
            free(resolved_path);
            break;
        }

        if (size_so_far + sb.st_size > cfg_cache_limit) {
            free(resolved_path);
            break;
        }

        size_so_far += sb.st_size;

        uint64_t pos = 0;
        bool last_extent_seen = false;
        while (pos < (uint64_t)sb.st_size && !last_extent_seen) {
            memset(fiemap, 0, sizeof(struct fiemap));
            fiemap->fm_start = pos;
            fiemap->fm_length = UINT64_MAX;
            fiemap->fm_flags = 0;
            fiemap->fm_extent_count = extent_buffer_elements;

            int ioctl_res = ioctl(fd, FS_IOC_FIEMAP, fiemap);
            if (ioctl_res != 0)
                break;

            for (uint32_t idx = 0; idx < fiemap->fm_mapped_extents; idx++) {
                struct fiemap_extent *ext = &fiemap->fm_extents[idx];

                pos = ext->fe_logical + ext->fe_length;
                if (ext->fe_flags & FIEMAP_EXTENT_LAST)
                    last_extent_seen = true;

                if (ext->fe_logical <= (uint64_t)sb.st_size) {
                    // Reduce .fe_length to match file size.
                    if (ext->fe_logical + ext->fe_length > (uint64_t)sb.st_size)
                        ext->fe_length = sb.st_size - ext->fe_logical;
                }

                struct sort_array_entry sae = {
                    .file_name = xstrdup(resolved_path),
                    .physical_pos = ext->fe_physical,
                    .file_offset = ext->fe_logical,
                    .extent_length = ext->fe_length,
                };

                utarray_push_back(&sort_array, &sae);
                LOG("%s: unsorted segment (%8zu, %7zu) path=%s", __func__,
                    sae.physical_pos, sae.extent_length, sae.file_name);
            }
        }

        free(resolved_path);
        close(fd);
    }

    dstate->cached_files_count = count;
    LOG("%s: cached_files_count=%zu", __func__, count);

    if (utarray_len(&sort_array) > 0)
        utarray_sort(&sort_array, sort_array_comparator);

    // Actual reading of the files.
    struct sort_array_entry *sa = utarray_front(&sort_array);
    for (size_t k = 0; k < utarray_len(&sort_array); k++) {
        LOG("%s: sorted segment (%8zu, %7zu) path=%s", __func__,
            sa[k].physical_pos, sa[k].extent_length, sa[k].file_name);
        int fd = real_open(sa[k].file_name, O_RDONLY);
        if (fd < 0)
            continue;

        static char buf[512 * 1024];
        ssize_t to_read = sa[k].extent_length;
        off_t ofs = sa[k].file_offset;
        while (to_read > 0) {
            ssize_t chunk_sz =
                to_read < (ssize_t)sizeof(buf) ? to_read : (ssize_t)sizeof(buf);
            ssize_t bytes_read = pread(fd, buf, chunk_sz, ofs);
            if (bytes_read == -1 && errno == EINTR) {
                // Try again.
                continue;
            }
            if (bytes_read <= 0) {
                // Either an error (-1) or an EOF (0).
                break;
            }
            to_read -= bytes_read;
            ofs += bytes_read;
        }
        close(fd);
    }

    free(fiemap);

    utstring_done(&fname);
    utarray_done(&sort_array);
    LOG("%s: returning", __func__);
}

PRECACHE_EXPORT
struct dirent *
readdir(DIR *dirp)
{
    struct dirent *res = NULL;
    struct dirp_to_state_mapping *dstate = NULL;

    LOG("%s: dirp=%p", __func__, dirp);
    ensure_initialized();

    lock();
    HASH_FIND_PTR(dirp_to_state_map, &dirp, dstate);
    if (!dstate) {
        // This should not happen: no record of this DIR instance. Which means
        // this DIR wasn't read in full yet. Forwarding to real_readdir() may
        // help.

        res = real_readdir(dirp);
        unlock();
        return res;
    }

    if (dstate->current_dirent == NULL) {
        // Nothing left on the list.
        res = NULL;
        goto done;
    }

    res = dstate->current_dirent->ent;
    const char *d_name = dstate->current_dirent->ent->d_name;
    LOG("%s:   d_name=%s", __func__, d_name);

    if (strcmp(d_name, ".") == 0 || strcmp(d_name, "..") == 0)
        goto done;

    if (dstate->fsm_state == RDT_STATE_do_precaching &&
        dstate->cached_files_count == 0)  //
    {
        LOG("%s:   caching...", __func__);
        cache_files(dstate);
        LOG("%s:   cached %d files", __func__, dstate->cached_files_count);
    }

    if (dstate->cached_files_count > 0)
        dstate->cached_files_count -= 1;

    switch (dstate->fsm_state) {
    case RDT_STATE_start:
        dstate->fsm_state = RDT_STATE_readdir1_open0;
        break;
    case RDT_STATE_readdir1_open0:
        dstate->fsm_state = RDT_STATE_skip;
        break;
    case RDT_STATE_readdir1_open1:
        dstate->fsm_state = RDT_STATE_readdir2_open1;
        break;
    case RDT_STATE_readdir2_open1:
        dstate->fsm_state = RDT_STATE_skip;
        break;
    case RDT_STATE_readdir2_open2:
        dstate->fsm_state = RDT_STATE_readdir3_open2;
        break;
    case RDT_STATE_readdir3_open2:
        dstate->fsm_state = RDT_STATE_skip;
        break;
    case RDT_STATE_do_precaching:
    case RDT_STATE_skip:
    default:
        break;
    }

done:
    if (dstate->current_dirent)
        dstate->current_dirent = dstate->current_dirent->next;

    unlock();

    return res;
}

PRECACHE_EXPORT
struct dirent64 *
readdir64(DIR *dirp)
{
    // When _DIRENT_MATCHES_DIRENT64 is 1, struct dirent and struct dirent64
    // have the same layout. This redirection depends on them being the same
    // type.
    return (struct dirent64 *)readdir(dirp);
}

static void
handle_closedir(DIR *dirp)
{
    lock();
    struct dirp_to_state_mapping *dstate = NULL;
    HASH_FIND_PTR(dirp_to_state_map, &dirp, dstate);
    if (dstate) {
        HASH_DEL(dirp_to_state_map, dstate);
        drain_dirent_list(dstate);
        free_dirp_to_state_mapping(dstate);
    }

    unlock();
}

PRECACHE_EXPORT
int
closedir(DIR *dirp)
{
    ensure_initialized();
    LOG("%s: dirp=%p", __func__, dirp);

    int res = real_closedir(dirp);
    handle_closedir(dirp);

    return res;
}

static void
handle_rewinddir(DIR *dirp)
{
    struct dirp_to_state_mapping *dstate = NULL;

    lock();
    HASH_FIND_PTR(dirp_to_state_map, &dirp, dstate);
    if (!dstate)
        goto done;

    // Calling rewinddir is similar to calling a separate opendir. Everything
    // starts over, so the state is also reset.
    dstate->fsm_state = RDT_STATE_start;

    // Start from the beginning of the list.
    dstate->current_dirent = dstate->dirent_list;

done:
    unlock();
}

PRECACHE_EXPORT
void
rewinddir(DIR *dirp)
{
    ensure_initialized();
    LOG("%s: dirp=%p", __func__, dirp);

    real_rewinddir(dirp);
    handle_rewinddir(dirp);
}

static void
handle_openat(int atfd, const char *fname)
{
    size_t fname_len = strlen(fname);
    if (atfd != AT_FDCWD) {
        // TODO: handle (atfd!=AT_FDCWD) case.
        return;
    }

    for (struct dirp_to_state_mapping *it = dirp_to_state_map; it != NULL;
         it = it->hh.next)  //
    {
        // Path must begin with a stored directory name while the remaining
        // part should contain no slashes. That is true only for files in that
        // directory, but not for files deeper in hierarchy.
        size_t dirname_len = strlen(it->dirname);
        bool match = fname_len > dirname_len + 1 &&
                     strncmp(fname, it->dirname, dirname_len) == 0 &&
                     strchr(fname + dirname_len + 1, '/') == NULL;

        if (!match)
            continue;

        switch (it->fsm_state) {
        case RDT_STATE_start:
            it->fsm_state = RDT_STATE_skip;
            break;
        case RDT_STATE_readdir1_open0:
            it->fsm_state = RDT_STATE_readdir1_open1;
            break;
        case RDT_STATE_readdir1_open1:
            it->fsm_state = RDT_STATE_skip;
            break;
        case RDT_STATE_readdir2_open1:
            it->fsm_state = RDT_STATE_readdir2_open2;
            break;
        case RDT_STATE_readdir2_open2:
            it->fsm_state = RDT_STATE_skip;
            break;
        case RDT_STATE_readdir3_open2:
            it->fsm_state = RDT_STATE_do_precaching;
            break;
        case RDT_STATE_do_precaching:
        case RDT_STATE_skip:
        default:
            break;
        }

        // There could be multiple simultaneously active opendir's for the same
        // directory, so multiple matches are possible. Currenly, all but first
        // seen one are ignored.
        // TODO: handling multiple opendir's for the same directory?
        break;
    }
}

static int
do_openat(int (*open_func)(int atfd, const char *fname, int oflag, ...),
          int atfd, const char *fname, int oflag, int mode)

{
    int fd = open_func(atfd, fname, oflag, mode);
    LOG("  open_func in do_openat returns %d", fd);
    handle_openat(atfd, fname);
    return fd;
}

PRECACHE_EXPORT
int
openat(int atfd, const char *fname, int oflag, ...)
{
    int mode = get_mode();
    LOG("%s: atfd=%d, fname=%s, oflag=%d, mode=%d", __func__, atfd, fname,
        oflag, mode);
    ensure_initialized();
    return do_openat(real_openat, atfd, fname, oflag, mode);
}

PRECACHE_EXPORT
int
open(const char *fname, int oflag, ...)
{
    int mode = get_mode();
    LOG("%s: fname=%s, oflag=%d, mode=%d", __func__, fname, oflag, mode);
    ensure_initialized();
    return do_openat(real_openat, AT_FDCWD, fname, oflag, mode);
}

__attribute__((destructor)) static void
destructor(void)
{
    lock();
    encfs_mapper_cleanup();
    clear_dirp_to_state_map();
    unlock();
}
