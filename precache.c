// Copyright 2021  Rinat Ibragimov
// SPDX-License-Identifier: MIT

#include "encfs_mapper.h"
#include "intercepted_functions.h"
#include "mem.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <utlist.h>

#define STR_AND_LENGTH(s) (s), sizeof(s) - 1

struct segment {
    char *file_name;
    uint64_t physical_pos;
    uint64_t file_offset;
    uint64_t extent_length;
    struct segment *prev, *next;
};

static int
segment_comparator(const void *a, const void *b)
{
    const struct segment *a_ = a;
    const struct segment *b_ = b;

    return (a_->physical_pos < b_->physical_pos)
               ? -1
               : (a_->physical_pos > b_->physical_pos);
}

static void *
memappend(void *p, const void *str, size_t len)
{
    return memcpy(p, str, len) + len;
}

static void *
memfill(void *p, char c, size_t count)
{
    return memset(p, c, count) + count;
}

static void
display_progress_unthrottled(const char *name, size_t current, size_t total)
{
    struct winsize ws;
    int name_len = strlen(name);
    int whole_width = (ioctl(1, TIOCGWINSZ, &ws) == 0 ? ws.ws_col : 80) - 1;
    int numbers_width = snprintf(NULL, 0, "%zu/%zu", current, total);
    int bar_width =
        whole_width - name_len - (int)strlen(" [] ") - numbers_width;
    if (numbers_width < 0 || bar_width < 1 || total < 1 || current > total)
        return;
    int fill_width = current * bar_width / total;

    char *buf = xmalloc(whole_width + 1);
    char *ptr = buf;

    ptr = memappend(ptr, name, name_len);
    ptr = memappend(ptr, STR_AND_LENGTH(" ["));
    ptr = memfill(ptr, '=', fill_width);
    ptr = memfill(ptr, ' ', bar_width - fill_width);
    ptr = memappend(ptr, STR_AND_LENGTH("] "));
    assert(ptr <= buf + whole_width);
    ptr +=
        snprintf(ptr, buf + whole_width + 1 - ptr, "%zu/%zu", current, total);
    assert(ptr == buf + whole_width);

    printf("\r%.*s", (int)whole_width, buf);
    fflush(stdout);
    free(buf);
}

static void
display_progress_throttled(const char *name, size_t current, size_t total)
{
    const size_t HZ = 60;
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t ticks = (uint64_t)now.tv_sec * HZ +
                     (uint64_t)now.tv_nsec * HZ / (1000 * 1000 * 1000);
    static uint64_t last_ticks = 0;

    if (ticks == last_ticks)
        return;

    last_ticks = ticks;
    display_progress_unthrottled(name, current, total);
}

static void
enumerate_file_segments(const char *fname, struct segment **segments,
                        size_t *file_segment_count)
{
    uint32_t extent_buffer_elements = 1000;
    struct fiemap *fiemap = NULL;

    if (file_segment_count)
        *file_segment_count = 0;

    fiemap = xcalloc(1, sizeof(struct fiemap) + sizeof(struct fiemap_extent) *
                                                    extent_buffer_elements);

    char *resolved_path = encfs_mapper_resolve_path(fname);
    if (!resolved_path)
        goto err_1;

    int fd = open(resolved_path, O_RDONLY);
    if (fd < 0)
        goto err_2;

    struct stat sb;
    int res = fstat(fd, &sb);
    if (res != 0)
        goto err_3;

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

            struct segment *seg = xmalloc(sizeof(*seg));

            seg->file_name = xstrdup(resolved_path);
            seg->physical_pos = ext->fe_physical;
            seg->file_offset = ext->fe_logical;
            seg->extent_length = ext->fe_length;

            DL_APPEND(*segments, seg);
        }

        if (file_segment_count)
            *file_segment_count += fiemap->fm_mapped_extents;
    }

err_3:
    close(fd);
err_2:
    free(resolved_path);
err_1:
    free(fiemap);
    return;
}

static void
read_segment(struct segment *it)
{
    int fd = open(it->file_name, O_RDONLY);
    if (fd < 0)
        return;

    static char buf[512 * 1024];
    ssize_t to_read = it->extent_length;
    off_t ofs = it->file_offset;
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

static void
free_segment_list(struct segment **segments)
{
    while (*segments) {
        struct segment *it = *segments;
        DL_DELETE(*segments, it);
        free(it->file_name);
        free(it);
    }
}

int
main(int argc, char *argv[])
{
    struct segment *segments = NULL;

    ensure_initialized();

    encfs_mapper_force_refresh_mounts();

    size_t total_segment_count = 0;
    for (int k = 1; k < argc; k++) {
        size_t file_segment_count;
        display_progress_throttled("mapping", k - 1, argc);
        enumerate_file_segments(argv[k], &segments, &file_segment_count);
        total_segment_count += file_segment_count;
    }
    display_progress_unthrottled("mapping", argc, argc);

    if (segments)
        DL_SORT(segments, segment_comparator);
    printf("\n");

    int count = 0;
    for (struct segment *it = segments; it != NULL; it = it->next) {
        display_progress_throttled("reading", ++count, total_segment_count);
        read_segment(it);
    }
    display_progress_unthrottled("reading", total_segment_count,
                                 total_segment_count);
    printf("\n");

    free_segment_list(&segments);

    return 0;
}
