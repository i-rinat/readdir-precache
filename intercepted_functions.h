// Copyright 2021  Rinat Ibragimov
// SPDX-License-Identifier: MIT

#pragma once

#include <dirent.h>
#include <stdlib.h>

extern int (*real_open)(const char *fname, int oflag, ...);
extern int (*real_open64)(const char *fname, int oflag, ...);
extern int (*real_openat)(int atfd, const char *fname, int oflag, ...);
extern int (*real_openat64)(int atfd, const char *fname, int oflag, ...);
extern int (*real_close)(int fd);
extern ssize_t (*real_read)(int fd, const void *buf, size_t count);
extern DIR *(*real_opendir)(const char *name);
extern struct dirent *(*real_readdir)(DIR *dirp);
extern struct dirent64 *(*real_readdir64)(DIR *dirp);
extern int (*real_closedir)(DIR *dirp);
extern void (*real_rewinddir)(DIR *dirp);

void
ensure_initialized(void);
