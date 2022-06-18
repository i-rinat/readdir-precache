// Copyright 2021-2022  Rinat Ibragimov
// SPDX-License-Identifier: MIT

#define _GNU_SOURCE
#include "intercepted_functions.h"
#include "mem.h"
#include "progress.h"
#include "segments.h"
#include "utils.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <utlist.h>
#include <utstring.h>

struct scan_task {
    char *dir_name;
    struct scan_task *prev, *next;
};

struct linux_dirent64 {
    ino64_t d_ino;
    off64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

static void
free_task_list(struct scan_task **tasks)
{
    while (*tasks) {
        struct scan_task *t = *tasks;
        DL_DELETE(*tasks, t);
        free(t->dir_name);
        free(t);
    }
}

static void
append_task(struct scan_task **tasks, const char *dir_name)
{
    struct scan_task *t = xmalloc(sizeof(*t));
    t->dir_name = xstrdup(dir_name);
    DL_APPEND(*tasks, t);
}

static void
read_segment(int fd, struct segment *seg, size_t *bytes_in_segment)
{
    static char buf[512 * 1024];
    ssize_t to_read = seg->extent_length;
    off_t ofs = seg->physical_pos;

    if (bytes_in_segment)
        *bytes_in_segment = 0;

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
}

static void
derive_new_tasks(const char *dir_name, dev_t root_dir_st_dev,
                 struct scan_task **next_tasks)
{
    size_t dir_name_len = strlen(dir_name);
    int dir_name_ends_with_slash =
        dir_name_len > 0 && dir_name[dir_name_len - 1] == '/';
    char buf[128 * 1024];
    int dir_fd = open(dir_name, O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) {
        fprintf(stderr, "\nError: can't open directory \"%s\"\n", dir_name);
        return;
    }

    UT_string fname;
    utstring_init(&fname);

    while (1) {
        int nread = syscall(SYS_getdents64, dir_fd, buf, sizeof(buf));
        if (nread == -1)
            break;

        if (nread == 0)
            break;

        int pos = 0;
        while (pos < nread) {
            struct linux_dirent64 *de = (struct linux_dirent64 *)(buf + pos);

            do {
                if (de->d_type != DT_DIR)
                    break;

                if (strcmp(de->d_name, ".") == 0 ||
                    strcmp(de->d_name, "..") == 0)  //
                {
                    break;
                }

                struct stat sb;
                int fstatat_ret =
                    fstatat(dir_fd, de->d_name, &sb, AT_SYMLINK_NOFOLLOW);
                if (fstatat_ret != 0 || sb.st_dev != root_dir_st_dev)
                    break;

                utstring_clear(&fname);
                utstring_printf(&fname, "%s%s%s", dir_name,
                                dir_name_ends_with_slash ? "" : "/",
                                de->d_name);

                append_task(next_tasks, utstring_body(&fname));

            } while (0);

            pos += de->d_reclen;
        }
    }

    utstring_done(&fname);
    close(dir_fd);
}

static size_t
get_task_count(struct scan_task *task_list)
{
    size_t count = 0;
    struct scan_task *task = task_list;
    while (task != NULL) {
        count++;
        task = task->next;
    }
    return count;
}

static int
common_prefix_length(const char *s1, const char *s2)
{
    int pos = 0;
    while (s1[pos] && s2[pos] && s1[pos] == s2[pos])
        pos++;
    return pos;
}

static char *
guess_device_for_path(const char *path)
{
    UT_string proc_mounts;
    utstring_init(&proc_mounts);

    int r = file_get_contents("/proc/mounts", &proc_mounts);
    if (r != 0)
        goto err;

    char *const proc_mounts_start = utstring_body(&proc_mounts);
    char *const proc_mounts_end =
        proc_mounts_start + utstring_len(&proc_mounts);

    int selected_mount_path_length = 0;
    char *selected_device_path = NULL;

    char *line_start = proc_mounts_start;
    while (line_start < proc_mounts_end) {
        char *line_end = strchr(line_start, '\n');
        *line_end = '\0';

        char *device_path = line_start;
        char *device_path_end = strchr(line_start, ' ');
        *device_path_end = '\0';

        char *mount_path = device_path_end + 1;
        char *mount_path_end = strchr(mount_path, ' ');
        *mount_path_end = '\0';

        if (device_path[0] == '/') {
            int path_common_len = common_prefix_length(mount_path, path);
            if (path_common_len > selected_mount_path_length) {
                selected_mount_path_length = path_common_len;
                selected_device_path = device_path;
            }
        }

        line_start = line_end + 1;
    }

    if (selected_device_path)
        selected_device_path = strdup(selected_device_path);

err:
    utstring_done(&proc_mounts);
    return selected_device_path;
}

int
main(int argc, char *argv[])
{
    ensure_initialized();

    if (argc < 2) {
        printf("Usage: precache-dir <root-dir> [raw-device]\n");
        return 2;
    }

    const char *root_dir = argv[1];
    char *raw_device_file_name = NULL;

    if (argc == 2) {
        // No raw-device file was provided. Try to guess.
        raw_device_file_name = guess_device_for_path(root_dir);
        printf("Raw device guessed by examining /proc/mounts: %s\n",
               raw_device_file_name);
    } else {
        raw_device_file_name = strdup(argv[2]);
    }

    int raw_device_fd = open(raw_device_file_name, O_RDONLY);
    if (raw_device_fd < 0) {
        fprintf(stderr, "Error: can't open raw device file %s\n",
                raw_device_file_name);
        goto err;
    }

    struct stat sb;
    int lstat_ret = lstat(root_dir, &sb);
    if (lstat_ret != 0) {
        fprintf(stderr, "Error: can't stat %s\n", root_dir);
        close(raw_device_fd);
        goto err;
    }

    dev_t root_dir_st_dev = sb.st_dev;
    size_t total_bytes_read = 0;

    struct scan_task *current_tasks = NULL;
    append_task(&current_tasks, root_dir);

    while (current_tasks != NULL) {
        struct segment *segments = NULL;
        size_t segment_count = 0;
        size_t task_idx = 0;

        size_t current_task_count = get_task_count(current_tasks);

        // Enumerate segments of all currently processed directories.
        for (struct scan_task *task = current_tasks; task != NULL;
             task = task->next)  //
        {
            size_t file_segment_count = 0;
            enumerate_file_segments(task->dir_name, &segments,
                                    &file_segment_count);
            segment_count += file_segment_count;
            display_progress_throttled("mapping directories", ++task_idx,
                                       current_task_count);
        }
        display_progress_unthrottled("mapping directories", current_task_count,
                                     current_task_count);
        printf("\n");

        // Sort and read data from the raw device.
        DL_SORT(segments, segment_comparator);
        size_t segment_idx = 0;
        for (struct segment *seg = segments; seg != NULL; seg = seg->next) {
            size_t bytes_in_segment = 0;
            read_segment(raw_device_fd, seg, &bytes_in_segment);
            display_progress_throttled("reading raw device", ++segment_idx,
                                       segment_count);
            total_bytes_read += bytes_in_segment;
        }
        display_progress_unthrottled("reading raw device", segment_count,
                                     segment_count);
        printf("\n");

        free_segment_list(&segments);

        struct scan_task *next_tasks = NULL;
        task_idx = 0;
        for (struct scan_task *task = current_tasks; task != NULL;
             task = task->next)  //
        {
            derive_new_tasks(task->dir_name, root_dir_st_dev, &next_tasks);
            display_progress_throttled("deriving new tasks", ++task_idx,
                                       current_task_count);
        }
        display_progress_unthrottled("deriving new tasks", current_task_count,
                                     current_task_count);
        printf("\n");

        free_task_list(&current_tasks);
        current_tasks = next_tasks;
    }

    free_task_list(&current_tasks);

    const size_t one_MiB = 1024 * 1024;
    printf("total data read: %zu MiB (%zu B)\n",
           (total_bytes_read + one_MiB - 1) / one_MiB, total_bytes_read);

    free(raw_device_file_name);
    return 0;

err:
    free(raw_device_file_name);
    return 1;
}
