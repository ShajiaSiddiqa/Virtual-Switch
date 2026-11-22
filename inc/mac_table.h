#ifndef MAC_TABLE_H
#define MAC_TABLE_H


#include <time.h>
#include <pthread.h>
#include "frame.h"
#include "mac_store.h" 
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
    uint16_t vlan;
    int port;
    int recovered; /* Retained hint: not a forwarding destination until source relearns. */
    struct timespec last_seen;
    struct MacEntry *next;
    uint_fast64_t last_used;
} MacEntry;

typedef struct {
    pthread_mutex_t lock;
    MacEntry *head;
} MacBucket;

typedef void (*MacMoveLogger)(void *context,
    const uint8_t mac[FRAME_MAC_SIZE], uint16_t vlan, int old_port, int new_port);

typedef int (*MacCheckpoint)(void *context, const MacStoreImage *image);

typedef struct {
    MacCheckpoint checkpoint;
    void *checkpoint_context;
    MacMoveLogger move_logger;
    void *move_log_context;
    MacBucket buckets[MAC_TABLE_BUCKETS];
    atomic_uint_fast64_t use_sequence;
    pthread_mutex_t capacity_lock;
    size_t entry_count;
} MacTable;

typedef struct {
    uint8_t mac[FRAME_MAC_SIZE];
    uint16_t vlan;
    int port;
} MacRecord;

// Initialize an empty table before any threads use it. Returns 0 on success or -1 on error. 
int mac_table_init(MacTable *table);
/* Configure only before starting any table users; context lives until they stop. */
void mac_table_set_move_logger(MacTable *table, MacMoveLogger callback, void *context);

int mac_table_lookup_vlan(MacTable *table, const uint8_t mac[FRAME_MAC_SIZE],
                          uint16_t vlan, int *port);
int mac_table_insert_vlan(MacTable *table, const uint8_t mac[FRAME_MAC_SIZE],
                          uint16_t vlan, int port);

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
/* Configure before any users. Callback runs with capacity/all buckets held;
   return 0 on success or -1 on failure, and never reenter this table.
   NULL keeps ordinary striped mode unchanged. */
void mac_table_set_checkpoint(MacTable *table, MacCheckpoint callback, void *context);
int mac_table_checkpoint(MacTable *table);
/* Restore into an initialized empty table before any users/checkpoint callback. */
int mac_table_restore(MacTable *table, const MacStoreImage *image);

#endif