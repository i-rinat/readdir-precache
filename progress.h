// Copyright 2021  Rinat Ibragimov
// SPDX-License-Identifier: MIT

#pragma once

#include <stdlib.h>

void
display_progress_unthrottled(const char *name, size_t current, size_t total);

void
display_progress_throttled(const char *name, size_t current, size_t total);
