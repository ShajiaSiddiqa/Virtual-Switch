#define _POSIX_C_SOURCE 200809L

#include "shm_stats.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// pthread functions return error numbers directly.Convert them into our API's -1 and errno convention. 
static int thread_result(int error)
{
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

int shm_stats_close(StatsHandle *handle)
{
    if (handle == NULL) {
        errno = EINVAL;
        return -1;
    }

    int saved = 0;

    if (handle->owner && shm_unlink(handle->name) == -1) 
        saved = errno;

    if (handle->shared != NULL && munmap(handle->shared, sizeof(SharedStats)) == -1 && saved == 0) 
        saved = errno;

    if (handle->fd != -1 && close(handle->fd) == -1 && saved == 0) 
        saved = errno;

    *handle = (StatsHandle){.fd = -1};

    if (saved != 0) {
        errno = saved;
        return -1;
    }
    return 0;
}

int shm_stats_create(StatsHandle *handle, size_t num_ports)
{
    if (handle == NULL || num_ports == 0 || num_ports > SHM_STATS_MAX_PORTS) {
        errno = EINVAL;
        return -1;
    }

    *handle = (StatsHandle){.fd = -1};

    const char *name = getenv("VSWITCH_STATS");
    if (name == NULL)
        name = SHM_STATS_NAME;
    if (name[0] != '/' || name[1] == '\0' || strchr(name + 1, '/') != NULL ||
        strlen(name) >= sizeof(handle->name)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(handle->name, name, strlen(name) + 1);
    handle->fd = shm_open(handle->name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (handle->fd == -1) 
        return -1;

    handle->owner = 1;

    if (ftruncate(handle->fd, (off_t)sizeof(SharedStats)) == -1) 
        goto fail;

    void *mapping = mmap(NULL, sizeof(SharedStats), PROT_READ | PROT_WRITE, MAP_SHARED, handle->fd, 0);
    if (mapping == MAP_FAILED) 
        goto fail;

    handle->shared = mapping;
    memset(handle->shared, 0, sizeof(SharedStats));

    pthread_mutexattr_t attr;
    if (thread_result(pthread_mutexattr_init(&attr)) == -1) 
        goto fail;

    int error = pthread_mutexattr_setpshared( &attr, PTHREAD_PROCESS_SHARED);

    if (error == 0) 
        error = pthread_mutex_init(&handle->shared->mutex, &attr);

    int attr_error = pthread_mutexattr_destroy(&attr);
    if (thread_result(error != 0 ? error : attr_error) == -1) 
        goto fail;

    handle->shared->num_ports = (uint32_t)num_ports;
    handle->shared->version = SHM_STATS_VERSION;
    return 0;

fail:
    {
        int saved = errno;
        int cleanup_result = shm_stats_close(handle);
        (void)cleanup_result;
        errno = saved;
        return -1;
    }
}

int shm_stats_attach(StatsHandle *handle)
{
    if (handle == NULL) {
        errno = EINVAL;
        return -1;
    }

    *handle = (StatsHandle){.fd = -1};
    const char *name = getenv("VSWITCH_STATS");
    if (name == NULL)
        name = SHM_STATS_NAME;
    if (name[0] != '/' || name[1] == '\0' || strchr(name + 1, '/') != NULL ||
        strlen(name) >= sizeof(handle->name)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(handle->name, name, strlen(name) + 1);
    handle->fd = shm_open(handle->name, O_RDWR, 0);
    if (handle->fd == -1) 
        return -1;

    struct stat info;
    if (fstat(handle->fd, &info) == -1) 
        goto fail;

    if (info.st_size != (off_t)sizeof(SharedStats)) {
        errno = EPROTO;
        goto fail;
    }

    void *mapping = mmap(NULL, sizeof(SharedStats), PROT_READ | PROT_WRITE, MAP_SHARED, handle->fd, 0);
    if (mapping == MAP_FAILED) 
        goto fail;

    handle->shared = mapping;

    if (handle->shared->version != SHM_STATS_VERSION || handle->shared->num_ports == 0 || handle->shared->num_ports > SHM_STATS_MAX_PORTS) {
        errno = EPROTO;
        goto fail;
    }

    return 0;

fail:
    {
        int saved = errno;
        int cleanup_result = shm_stats_close(handle);
        (void)cleanup_result;
        errno = saved;
        return -1;
    }
}

int shm_stats_add(StatsHandle *handle, size_t port, StatsCounter counter)
{
    if (handle == NULL || handle->shared == NULL || port >= handle->shared->num_ports || 
        counter < STATS_FRAMES_IN || counter > STATS_FRAMES_MIRRORED) {
        errno = EINVAL;
        return -1;
    }

    SharedStats *shared = handle->shared;

    if (thread_result(pthread_mutex_lock(&shared->mutex)) == -1) 
        return -1;

    PortStats *stats = &shared->ports[port];

    switch (counter) {
    case STATS_FRAMES_IN:
        stats->frames_in++;
        break;
    case STATS_FRAMES_OUT:
        stats->frames_out++;
        break;
    case STATS_FRAMES_DROPPED:
        stats->frames_dropped++;
        break;
    case STATS_FRAMES_MIRRORED:
        stats->frames_mirrored++;
        break;
    }

    return thread_result(pthread_mutex_unlock(&shared->mutex));
}

int shm_stats_snapshot(StatsHandle *handle, PortStats *ports, size_t capacity, size_t *num_ports)
{
    if (handle == NULL || handle->shared == NULL || ports == NULL || num_ports == NULL ||
        capacity < handle->shared->num_ports) {
        errno = EINVAL;
        return -1;
    }

    SharedStats *shared = handle->shared;

    if (thread_result(pthread_mutex_lock(&shared->mutex)) == -1) 
        return -1;

    *num_ports = shared->num_ports;
    memcpy(ports, shared->ports, *num_ports * sizeof(*ports));

    return thread_result(pthread_mutex_unlock(&shared->mutex));
}