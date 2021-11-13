// Copyright 2021  Rinat Ibragimov
// SPDX-License-Identifier: MIT

#include "segments.h"
#include "encfs_mapper.h"
#include "mem.h"
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utlist.h>

void
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

void
free_segment_list(struct segment **segments)
{
    while (*segments) {
        struct segment *it = *segments;
        DL_DELETE(*segments, it);
        free(it->file_name);
        free(it);
    }
}
