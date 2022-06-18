// Copyright 2022  Rinat Ibragimov
// SPDX-License-Identifier: MIT

#include "intercepted_functions.h"
#include "utils.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int
file_get_contents(const char *file_name, UT_string *body)
{
    char buf[4096];
    int fd = real_open(file_name, O_RDONLY);
    if (fd == -1)
        goto err_1;

    utstring_clear(body);

    size_t pos = 0;
    while (1) {
        ssize_t bytes_read = pread(fd, buf, sizeof(buf), pos);
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            goto err_2;
        }
        if (bytes_read == 0) {
            // Sudden file size change?
            break;
        }

        utstring_bincpy(body, buf, bytes_read);
        pos += bytes_read;
    }

    close(fd);
    return 0;

err_2:
    utstring_clear(body);
    close(fd);
err_1:
    return -1;
}
