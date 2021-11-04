// Copyright 2021  Rinat Ibragimov
// SPDX-License-Identifier: MIT

#pragma once

#include <stdlib.h>
#include <string.h>

#define precache_oom() exit(-1)

static inline char *
xstrdup(const char *src)
{
    char *dst = strdup(src);
    if (!dst)
        precache_oom();
    return dst;
}

static inline void *
xmalloc(size_t sz)
{
    void *ptr = malloc(sz);
    if (!ptr)
        precache_oom();
    return ptr;
}

static inline void *
xcalloc(size_t n, size_t sz)
{
    void *ptr = calloc(n, sz);
    if (!ptr)
        precache_oom();
    return ptr;
}
