#ifndef MAC_STORE_H
#define MAC_STORE_H

#include <stddef.h>
#include <stdint.h>
#include "frame.h"

#define MAC_STORE_CAPACITY 256u
#define MAC_STORE_MAX_PORTS 64u

/* Plain values only: no heap pointers or process-local linked lists. */
typedef struct {
    uint8_t mac[FRAME_MAC_SIZE];
    uint16_t vlan;
    int32_t port;                  /* zero-based stable slot */
    int64_t last_seen_seconds;     /* CLOCK_MONOTONIC on this machine */
    int32_t last_seen_nanoseconds;
    uint64_t last_used;
} MacStoreEntry;

typedef struct {
    uint64_t generation;           /* incremented by each committed publish */
    uint64_t use_sequence;         /* LRU sequence associated with this image */
    uint32_t count;
    MacStoreEntry entries[MAC_STORE_CAPACITY];
} MacStoreImage;

typedef struct {
    int fd;
    void *mapping;
    size_t mapping_size;
} MacStore;

/* Create exclusively; supervisor calls this before starting workers. */
int mac_store_create(MacStore *store, const char *name);
/* Attach only after create has returned successfully. */
int mac_store_attach(MacStore *store, const char *name);
/* Publish a complete recovery image atomically relative to readers/crashes. */
int mac_store_publish(MacStore *store, const MacStoreEntry *entries,
                      size_t count, uint64_t use_sequence);
int mac_store_read(MacStore *store, MacStoreImage *image);
/* Close never unlinks: the supervisor owns the object's lifetime. */
int mac_store_close(MacStore *store);
int mac_store_unlink(const char *name);

#ifdef MAC_STORE_TESTING
/* Test-only: notify then stop while holding the mutex for parent SIGKILL. */
int mac_store_test_hold(MacStore *store, int commit_first, int notify_fd);
#endif
#endif
