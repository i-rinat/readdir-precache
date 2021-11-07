// Copyright 2021  Rinat Ibragimov
// SPDX-License-Identifier: MIT

#pragma once

int
encfs_mapper_force_refresh_mounts(void);

int
encfs_mapper_refresh_mounts(const char *current_path);

char *
encfs_mapper_resolve_path(const char *src_path);

void
encfs_mapper_cleanup(void);
