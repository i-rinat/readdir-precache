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
#include <string.h>
#include <unistd.h>
#include <utlist.h>

static void
read_segment(struct segment *it, size_t *bytes_in_segment)
{
    int fd = open(it->file_name, O_RDONLY);
    if (fd < 0)
        return;

    if (bytes_in_segment)
        *bytes_in_segment = 0;

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
        if (bytes_in_segment)
            *bytes_in_segment += bytes_read;
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

    if (!isatty(fileno(stdin))) {
        int file_count = argc;
        char line[128 * 1024];

        while (fgets(line, sizeof(line), stdin)) {
            size_t line_len = strlen(line);
            while (line_len > 0 && line[line_len - 1] == '\n') {
                line[line_len - 1] = '\0';
                line_len -= 1;
            }
            file_count += 1;

            size_t file_segment_count;
            display_progress_throttled("mapping", file_count, file_count);
            enumerate_file_segments(line, &segments, &file_segment_count);
            total_segment_count += file_segment_count;
        }
        display_progress_unthrottled("mapping", file_count, file_count);
    }

    if (segments)
        DL_SORT(segments, segment_comparator);
    printf("\n");

    size_t total_bytes_read = 0;
    int count = 0;
    for (struct segment *it = segments; it != NULL; it = it->next) {
        size_t bytes_in_segment = 0;
        display_progress_throttled("reading", ++count, total_segment_count);
        read_segment(it, &bytes_in_segment);
        total_bytes_read += bytes_in_segment;
    }
    display_progress_unthrottled("reading", total_segment_count,
                                 total_segment_count);
    printf("\n");

    free_segment_list(&segments);

    const size_t one_MiB = 1024 * 1024;
    printf("total data read: %zu MiB (%zu B)\n",
           (total_bytes_read + one_MiB - 1) / one_MiB, total_bytes_read);

    return 0;
}
