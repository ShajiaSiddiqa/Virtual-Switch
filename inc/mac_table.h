#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <pthread.h>
#include "frame.h"  

#define MAC_TABLE_BUCKETS 16

typedef struct MacEntry {
    uint8_t mac[FRAME_MAC_SIZE];
    int port;
    struct MacEntry *next;
} MacEntry;

typedef struct {
    pthread_mutex_t lock;
    MacEntry *head;
} MacBucket;

typedef struct {
    MacBucket buckets[MAC_TABLE_BUCKETS];
} MacTable;

// Initialize an empty table before any threads use it. Returns 0 on success or -1 on error. 
int mac_table_init(MacTable *table);

// Free all entries and destroy the locks after all users have stopped. Returns 0 on success or -1 on error.
int mac_table_destroy(MacTable *table);

// Returns 1 if found, 0 if missing, or -1 on error. Writes the port to *port only when found. 
int mac_table_lookup(MacTable *table, const uint8_t mac[FRAME_MAC_SIZE], int *port);

// Learn a MAC address or update its port if it has moved. Returns 0 on success or -1 on error.
int mac_table_insert(MacTable *table, const uint8_t mac[FRAME_MAC_SIZE], int port);

#endif