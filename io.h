#ifndef IO_H
#define IO_H

#include <stddef.h>
#include <sys/types.h>

ssize_t read_full(int fd, void *buffer, size_t count);
int write_full(int fd, const void *buffer, size_t count);

#endif