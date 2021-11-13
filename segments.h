// Copyright 2021  Rinat Ibragimov
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

struct segment {
    char *file_name;
    uint64_t physical_pos;
    uint64_t file_offset;
    uint64_t extent_length;
    struct segment *prev, *next;
};

static inline int
segment_comparator(const void *a, const void *b)
{
    const struct segment *a_ = a;
    const struct segment *b_ = b;

    return (a_->physical_pos < b_->physical_pos)
               ? -1
               : (a_->physical_pos > b_->physical_pos);
}

void
enumerate_file_segments(const char *fname, struct segment **segments,
                        size_t *file_segment_count);

void
free_segment_list(struct segment **segments);
