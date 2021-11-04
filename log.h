// Copyright 2021  Rinat Ibragimov
// SPDX-License-Identifier: MIT

#pragma once

#if 0
#define LOG(fmt, ...) fprintf(stderr, "precache: " fmt "\n", __VA_ARGS__)
#define LOG_(fmt) fprintf(stderr, "precache: " fmt "\n")
#else
#define LOG(...)
#define LOG_(...)
#endif
