// Copyright 2021  Rinat Ibragimov
// SPDX-License-Identifier: MIT

#include "encfs_mapper.h"
#include "intercepted_functions.h"
#include "progress.h"
#include "segments.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <utlist.h>

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
