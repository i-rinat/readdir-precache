// Copyright 2021  Rinat Ibragimov
// SPDX-License-Identifier: MIT

#define _GNU_SOURCE
#include "intercepted_functions.h"
#include <dlfcn.h>
#include <pthread.h>

static pthread_once_t once_control = PTHREAD_ONCE_INIT;

int (*real_open)(const char *fname, int oflag, ...);
int (*real_open64)(const char *fname, int oflag, ...);
int (*real_openat)(int atfd, const char *fname, int oflag, ...);
int (*real_openat64)(int atfd, const char *fname, int oflag, ...);
int (*real_close)(int fd);
ssize_t (*real_read)(int fd, const void *buf, size_t count);
DIR *(*real_opendir)(const char *name);
struct dirent *(*real_readdir)(DIR *dirp);
struct dirent64 *(*real_readdir64)(DIR *dirp);
int (*real_closedir)(DIR *dirp);
void (*real_rewinddir)(DIR *dirp);

static void
initialize(void)
{
    real_open = dlsym(RTLD_NEXT, "open");
    real_open64 = dlsym(RTLD_NEXT, "open64");
    real_openat = dlsym(RTLD_NEXT, "openat");
    real_openat64 = dlsym(RTLD_NEXT, "openat64");
    real_read = dlsym(RTLD_NEXT, "read");
    real_close = dlsym(RTLD_NEXT, "close");
    real_opendir = dlsym(RTLD_NEXT, "opendir");
    real_readdir = dlsym(RTLD_NEXT, "readdir");
    real_readdir64 = dlsym(RTLD_NEXT, "readdir64");
    real_closedir = dlsym(RTLD_NEXT, "closedir");
    real_rewinddir = dlsym(RTLD_NEXT, "rewinddir");
}

void
ensure_initialized(void)
{
    pthread_once(&once_control, initialize);
}
