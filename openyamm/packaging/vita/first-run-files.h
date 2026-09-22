#ifndef OPENYAMM_FIRST_RUN_FILES_H
#define OPENYAMM_FIRST_RUN_FILES_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

// Runs only in the small bootstrap process, before game threads or the guest heap exist.
// Existing user files (including empty files) are never replaced. A failed copy never
// publishes a partial configuration. The caller supplies only fixed package filenames.
static int oy_install_missing_file(const char *source, const char *destination)
{
    struct stat status;
    if (stat(destination, &status) == 0) return 0;
    if (errno != ENOENT) return -1;

    char temporary[256];
    int length = snprintf(temporary, sizeof(temporary), "%s.first-run.tmp", destination);
    if (length < 0 || (size_t)length >= sizeof(temporary)) return -1;
    int input = open(source, O_RDONLY);
    if (input < 0) return -1;
    // This filename belongs exclusively to first-run initialization. Reuse after interruption.
    int output = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (output < 0) { close(input); return -1; }
    char buffer[4096];
    int result = 1;
    for (;;)
    {
        ssize_t count = read(input, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { result = -1; break; }
        if (count == 0) break;
        ssize_t offset = 0;
        while (offset < count)
        {
            ssize_t written = write(output, buffer + offset, count - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { result = -1; break; }
            offset += written;
        }
        if (result < 0) break;
    }
    close(input);
    if (close(output) != 0) result = -1;
    if (result > 0)
    {
        if (stat(destination, &status) == 0) result = 0;
        else if (errno != ENOENT || rename(temporary, destination) != 0) result = -1;
    }
    if (result <= 0) unlink(temporary);
    return result;
}

#endif
