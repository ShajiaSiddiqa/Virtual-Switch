#ifndef SHM_STATS_H
#define SHM_STATS_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define SHM_STATS_NAME     "/vswitch_stats"
#define SHM_STATS_MAX_PORTS 64
#define SHM_STATS_VERSION  1u

typedef struct {
    uint64_t frames_in;
    uint64_t frames_out;
    uint64_t frames_dropped;
    uint64_t frames_mirrored;
} PortStats;

typedef struct {
    uint32_t version;
    uint32_t num_ports;
    pthread_mutex_t mutex;
    PortStats ports[SHM_STATS_MAX_PORTS];
} SharedStats;

typedef struct {
    char name[256];
    int fd;
    int owner;
    SharedStats *shared;
} StatsHandle;

typedef enum {
    STATS_FRAMES_IN,
    STATS_FRAMES_OUT,
    STATS_FRAMES_DROPPED,
    STATS_FRAMES_MIRRORED
} StatsCounter;

int shm_stats_create(StatsHandle *handle, size_t num_ports);

// attach client to existing intiialized obj. 
int shm_stats_attach(StatsHandle *handle);

// incr one counter for zero-based port idx
int shm_stats_add(StatsHandle *handle, size_t port, StatsCounter counter);

// copy consistent snapshot into the caller's array.cap is the number of PortStats elems available.
int shm_stats_snapshot(StatsHandle *handle, PortStats *ports,size_t capacity, size_t *num_ports);

// unmap and close this process's handle.The creating owner also unlinks the shared-memory name.
int shm_stats_close(StatsHandle *handle);

#endif