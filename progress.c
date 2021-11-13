// Copyright 2021  Rinat Ibragimov
// SPDX-License-Identifier: MIT

#include "progress.h"
#include "mem.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

void
display_progress_unthrottled(const char *name, size_t current, size_t total)
{
    struct winsize ws;
    int name_len = strlen(name);
    int whole_width = (ioctl(1, TIOCGWINSZ, &ws) == 0 ? ws.ws_col : 80) - 1;
    int numbers_width = snprintf(NULL, 0, "%zu/%zu", current, total);
    int bar_width =
        whole_width - name_len - (int)strlen(" [] ") - numbers_width;
    if (numbers_width < 0 || bar_width < 1 || total < 1 || current > total)
        return;
    int fill_width = current * bar_width / total;

    char *buf = xmalloc(whole_width + 1);
    char *ptr = buf;

    ptr = memappend(ptr, name, name_len);
    ptr = memappend(ptr, STR_AND_LENGTH(" ["));
    ptr = memfill(ptr, '=', fill_width);
    ptr = memfill(ptr, ' ', bar_width - fill_width);
    ptr = memappend(ptr, STR_AND_LENGTH("] "));
    assert(ptr <= buf + whole_width);
    ptr +=
        snprintf(ptr, buf + whole_width + 1 - ptr, "%zu/%zu", current, total);
    assert(ptr == buf + whole_width);

    printf("\r%.*s", (int)whole_width, buf);
    fflush(stdout);
    free(buf);
}

void
display_progress_throttled(const char *name, size_t current, size_t total)
{
    const size_t HZ = 60;
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t ticks = (uint64_t)now.tv_sec * HZ +
                     (uint64_t)now.tv_nsec * HZ / (1000 * 1000 * 1000);
    static uint64_t last_ticks = 0;

    if (ticks == last_ticks)
        return;

    last_ticks = ticks;
    display_progress_unthrottled(name, current, total);
}
