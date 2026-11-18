#ifndef MAC_TABLE_H
#define MAC_TABLE_H


#include <time.h>
#include <pthread.h>
#include "frame.h" 
#include <stdatomic.h> 

#define MAC_TABLE_BUCKETS 16

#ifndef MAC_TABLE_CAPACITY
#define MAC_TABLE_CAPACITY 256
#endif

#ifndef MAC_TABLE_PORT_LIMIT
#define MAC_TABLE_PORT_LIMIT 4
#endif

#define MAC_TABLE_PORT_LIMIT_REACHED 1


typedef struct MacEntry {
    uint8_t mac[FRAME_MAC_SIZE];
    int port;
    struct timespec last_seen;
    struct MacEntry *next;
    uint_fast64_t last_used;
} MacEntry;

typedef struct {
    pthread_mutex_t lock;
    MacEntry *head;
} MacBucket;

typedef struct {
    MacBucket buckets[MAC_TABLE_BUCKETS];
    atomic_uint_fast64_t use_sequence;
    pthread_mutex_t capacity_lock;
    size_t entry_count;
} MacTable;

typedef struct {
    uint8_t mac[FRAME_MAC_SIZE];
    int port;
} MacRecord;

// Initialize an empty table before any threads use it. Returns 0 on success or -1 on error. 
int mac_table_init(MacTable *table);

// Free all entries and destroy the locks after all users have stopped. Returns 0 on success or -1 on error.
int mac_table_destroy(MacTable *table);

// Returns 1 if found, 0 if missing, or -1 on error. Writes the port to *port only when found. 
int mac_table_lookup(MacTable *table, const uint8_t mac[FRAME_MAC_SIZE], int *port);

// Learn a MAC address or update its port if it has moved. Returns 0 on success or -1 on error.
int mac_table_insert(MacTable *table, const uint8_t mac[FRAME_MAC_SIZE], int port);

// Returns the number removed, or -1 on error. 
int mac_table_expire(MacTable *table, unsigned int max_age_seconds);

// Remove mappings for a disconnected port. Returns a count or -1. 
int mac_table_remove_port(MacTable *table, int port);

int mac_table_snapshot(MacTable *table, MacRecord *records, size_t capacity, size_t *count);

int mac_table_clear(MacTable *table);

#endif