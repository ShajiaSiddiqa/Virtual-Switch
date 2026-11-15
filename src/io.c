#include "io.h"

#include <errno.h>
#include <unistd.h>

ssize_t read_full(int fd, void *buffer, size_t count)
{
    unsigned char *bytes = buffer;
    size_t received = 0;

    while (received < count) 
    {
        ssize_t result = read(fd, bytes + received, count - received);

        if (result == 0) 
        {
            if (received == 0) 
                return 0;

            errno = EPROTO;
            return -1;
        }

        if (result < 0) 
        {
            if (errno == EINTR) 
                continue;
            return -1;
        }

        received += (size_t)result;
    }

    return (ssize_t)received;
}

int write_full(int fd, const void *buffer, size_t count)
{
    const unsigned char *bytes = buffer;
    size_t sent = 0;

    while (sent < count) {
        ssize_t result = write(fd, bytes + sent, count - sent);

        if (result < 0) 
        {
            if (errno == EINTR) 
                continue;
            return -1;
        }

        if (result == 0) 
        {
            errno = EIO;
            return -1;
        }

        sent += (size_t)result;
    }

    return 0;
}