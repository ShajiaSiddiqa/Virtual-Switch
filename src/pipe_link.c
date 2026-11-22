#define _POSIX_C_SOURCE 200809L
#include "checked.h"
#include "pipe_link.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/stat.h>

static int nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags == -1 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
int pipe_link_destroy(PipeLink *link)
{
    if (link == NULL) { errno = EINVAL; return -1; }
    int saved = 0;
    int *fds[] = {&link->read_fd, &link->write_fd, &link->wake[0], &link->wake[1]};
    for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
        if (*fds[i] != -1 && close(*fds[i]) == -1 && saved == 0)
            saved = errno;
        *fds[i] = -1;
    }
    if (saved) { errno = saved; return -1; }
    return 0;
}
int pipe_link_adopt(PipeLink *link, int read_fd, int write_fd)
{
    if (link == NULL || read_fd < 0 || write_fd < 0 || read_fd == write_fd) {
        errno = EINVAL; return -1;
    }
    struct stat in, out;
    int in_flags = fcntl(read_fd, F_GETFL), out_flags = fcntl(write_fd, F_GETFL);
    if (in_flags == -1 || out_flags == -1 || fstat(read_fd, &in) == -1 ||
        fstat(write_fd, &out) == -1)
        return -1;
    if (!S_ISFIFO(in.st_mode) || !S_ISFIFO(out.st_mode) ||
        (in.st_dev == out.st_dev && in.st_ino == out.st_ino) ||
        (in_flags & O_ACCMODE) != O_RDONLY || (out_flags & O_ACCMODE) != O_WRONLY) {
        errno = EINVAL; return -1;
    }
    *link = (PipeLink){.read_fd=-1, .write_fd=-1, .wake={-1,-1}};
    atomic_init(&link->stopped, 0);
    if (pipe(link->wake) == -1)
        return -1;
    if (nonblocking(read_fd) == -1 || nonblocking(write_fd) == -1 ||
        nonblocking(link->wake[0]) == -1 || nonblocking(link->wake[1]) == -1) {
        int saved = errno;
        vsw_posix(close(link->wake[0]), "close"); vsw_posix(close(link->wake[1]), "close");
        link->wake[0] = link->wake[1] = -1;
        errno = saved;
        return -1;
    }
    link->read_fd = read_fd;
    link->write_fd = write_fd;
    return 0;
}

int pipe_link_pair(PipeLink endpoints[2])
{
    if (endpoints == NULL) { errno = EINVAL; return -1; }
    for (int i = 0; i < 2; i++) {
        endpoints[i].read_fd = endpoints[i].write_fd = -1;
        endpoints[i].wake[0] = endpoints[i].wake[1] = -1;
        atomic_init(&endpoints[i].stopped, 0);
    }
    int ab[2], ba[2];
    if (pipe(ab) == -1)
        return -1;
    if (pipe(ba) == -1) {
        int saved = errno;
        vsw_posix(close(ab[0]), "close"); vsw_posix(close(ab[1]), "close");
        errno = saved;
        return -1;
    }
    endpoints[0].read_fd = ba[0];
    endpoints[0].write_fd = ab[1];
    endpoints[1].read_fd = ab[0];
    endpoints[1].write_fd = ba[1];
    for (int i = 0; i < 2; i++) {
        if (pipe(endpoints[i].wake) == -1)
            goto fail;
        int fds[] = {endpoints[i].read_fd, endpoints[i].write_fd,
                     endpoints[i].wake[0], endpoints[i].wake[1]};
        for (size_t j = 0; j < sizeof(fds) / sizeof(fds[0]); j++) {
            if (nonblocking(fds[j]) == -1)
                goto fail;
        }
    }
    return 0;
fail:
    {
        int saved = errno;
        vsw_zero(pipe_link_destroy(&endpoints[0]), "pipe_link_destroy");
        vsw_zero(pipe_link_destroy(&endpoints[1]), "pipe_link_destroy");
        errno = saved;
        return -1;
    }
}

static int wait_ready(PipeLink *link, int fd, short events)
{
    struct pollfd descriptors[2] = {
        {.fd = fd, .events = events},
        {.fd = link->wake[0], .events = POLLIN}
    };
    for (;;) {
        if (atomic_load(&link->stopped)) { errno = ECANCELED; return -1; }
        int ready = poll(descriptors, 2, -1);
        if (ready == -1 && errno == EINTR)
            continue;
        if (ready == -1)
            return -1;
        if (atomic_load(&link->stopped) || descriptors[1].revents != 0) {
            errno = ECANCELED;
            return -1;
        }
        if (descriptors[0].revents & POLLNVAL) { errno = EBADF; return -1; }
        /* HUP/ERR must reach read/write so buffered bytes/EOF/EPIPE are handled. */
        if (descriptors[0].revents & (events | POLLHUP | POLLERR))
            return 0;
    }
}
static int read_exact(PipeLink *link, void *buffer, size_t length)
{
    uint8_t *bytes = buffer;
    size_t done = 0;
    while (done < length) {
        if (wait_ready(link, link->read_fd, POLLIN) == -1)
            return -1;
        ssize_t result = read(link->read_fd, bytes + done, length - done);
        if (result > 0) { done += (size_t)result; continue; }
        if (result == 0) {
            if (done == 0)
                return 0;
            errno = EPROTO;
            return -1;
        }
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
    }
    return 1;
}
static int write_exact(PipeLink *link, const void *buffer, size_t length)
{
    const uint8_t *bytes = buffer;
    size_t done = 0;
    while (done < length) {
        if (wait_ready(link, link->write_fd, POLLOUT) == -1)
            return -1;
        ssize_t result = write(link->write_fd, bytes + done, length - done);
        if (result > 0) { done += (size_t)result; continue; }
        if (result == 0) { errno = EIO; return -1; }
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
    }
    return 0;
}
int pipe_link_receive(PipeLink *link, FrameHeader *header,
                      uint8_t payload[FRAME_MAX_PAYLOAD])
{
    if (link == NULL || header == NULL || payload == NULL) { errno = EINVAL; return -1; }
    uint8_t wire[FRAME_HEADER_SIZE];
    int result = read_exact(link, wire, sizeof(wire));
    if (result <= 0)
        return result;
    if (frame_deserialize(wire, sizeof(wire), header) != 0) { errno = EPROTO; return -1; }
    if (header->length == 0)
        return 1;
    result = read_exact(link, payload, header->length);
    if (result == 0) { errno = EPROTO; return -1; }
    return result;
}
int pipe_link_send(PipeLink *link, const FrameHeader *header, const uint8_t *payload)
{
    if (link == NULL || header == NULL || (header->length && payload == NULL)) {
        errno = EINVAL; return -1;
    }
    if (atomic_load(&link->stopped)) { errno = ECANCELED; return -1; }
    uint8_t wire[FRAME_HEADER_SIZE];
    if (frame_serialize(header, wire, sizeof(wire)) != 0) { errno = EINVAL; return -1; }
    if (write_exact(link, wire, sizeof(wire)) == -1)
        return -1;
    return write_exact(link, payload, header->length);
}
int pipe_link_stop(PipeLink *link)
{
    if (link == NULL) { errno = EINVAL; return -1; }
    if (atomic_exchange(&link->stopped, 1))
        return 0;
    ssize_t result;
    do { result = write(link->wake[1], "S", 1); }
    while (result == -1 && errno == EINTR);
    if (result == 1 || (result == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)))
        return 0;
    return -1;
}
