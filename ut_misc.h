// Copyright 2021  Rinat Ibragimov
// SPDX-License-Identifier: MIT

#pragma once

#define HASH_FIND_UINT64(head, findint, out)                                   \
    HASH_FIND(hh, head, findint, sizeof(uint64_t), out)

#define HASH_ADD_UINT64(head, intfield, add)                                   \
    HASH_ADD(hh, head, intfield, sizeof(uint64_t), add)

#define HASH_REPLACE_UINT64(head, intfield, add, replaced)                     \
    HASH_REPLACE(hh, head, intfield, sizeof(uint64_t), add, replaced)

#define utstring_truncate(s, l)                                                \
    do {                                                                       \
        if ((size_t)(l) < (s)->i) {                                            \
            (s)->i = (size_t)(l);                                              \
            (s)->d[(size_t)(l)] = '\0';                                        \
        }                                                                      \
    } while (0)

#define utarray_index(a, type, idx) ((type *)((a)->d))[idx]

#define utstring_steal_data(s) ((s)->d)
