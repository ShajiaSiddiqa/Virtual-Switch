#define _POSIX_C_SOURCE 200809L
#include "mac_table.h"

#include <stdio.h>  
#include <stdlib.h>  
#include <string.h>  

static size_t bucket_index(const uint8_t mac[FRAME_MAC_SIZE])
{
    unsigned int hash = 0;

    for (size_t i = 0; i < FRAME_MAC_SIZE; i++) 
    {
        hash = hash * 31u + mac[i];
    }

    return hash % MAC_TABLE_BUCKETS;
}

// caller hold bucket`s lock. 
static MacEntry *find_entry(MacBucket *bucket, const uint8_t mac[FRAME_MAC_SIZE])
{
    for (MacEntry *entry = bucket->head; entry != NULL; entry = entry->next) 
    {
        if (memcmp(entry->mac, mac, FRAME_MAC_SIZE) == 0) 
            return entry;
    }

    return NULL;
}

int mac_table_init(MacTable *table)
{
    if (table == NULL) 
        return -1;

    for (size_t i = 0; i < MAC_TABLE_BUCKETS; i++) 
    {
        table->buckets[i].head = NULL;

        if (pthread_mutex_init(&table->buckets[i].lock, NULL) != 0) 
        {
            while (i > 0) 
            {
                i--;
                pthread_mutex_destroy(&table->buckets[i].lock);
            }
            return -1;
        }
    }

    return 0;
}

int mac_table_destroy(MacTable *table)
{
    if (table == NULL) 
        return -1;

    int result = 0;

    for (size_t i = 0; i < MAC_TABLE_BUCKETS; i++) 
    {
        MacBucket *bucket = &table->buckets[i];

        if (pthread_mutex_destroy(&bucket->lock) != 0) 
        {
            result = -1;
            continue;
        }

        while (bucket->head != NULL) 
        {
            MacEntry *entry = bucket->head;
            bucket->head = entry->next;
            free(entry);
        }
    }

    return result;
}

int mac_table_lookup(MacTable *table, const uint8_t mac[FRAME_MAC_SIZE], int *port)
{
    if (table == NULL || mac == NULL || port == NULL) 
        return -1;

    MacBucket *bucket = &table->buckets[bucket_index(mac)];
    if (pthread_mutex_lock(&bucket->lock) != 0) 
        return -1;

    MacEntry *entry = find_entry(bucket, mac);
    int found = 0;

    if (entry != NULL) 
    {
        *port = entry->port;
        found = 1;
    }

    pthread_mutex_unlock(&bucket->lock);
    return found;
}

int mac_table_insert(MacTable *table, const uint8_t mac[FRAME_MAC_SIZE], int port)
{
    if (table == NULL || mac == NULL || port < 0) {
        return -1;
    }

    MacBucket *bucket = &table->buckets[bucket_index(mac)];
    if (pthread_mutex_lock(&bucket->lock) != 0) 
        return -1;
    
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) 
    {
        pthread_mutex_unlock(&bucket->lock);
        return -1;
    }

    MacEntry *entry = find_entry(bucket, mac);

    if (entry != NULL) 
    {
        int old_port = entry->port;
        entry->port = port;
        entry->last_seen = now;
        pthread_mutex_unlock(&bucket->lock);

        if (old_port != port) 
            printf("MAC moved: port %d -> port %d\n", old_port, port);

        return 0;
    }

    entry = malloc(sizeof(*entry));
    if (entry == NULL) 
    {
        pthread_mutex_unlock(&bucket->lock);
        return -1;
    }

    memcpy(entry->mac, mac, FRAME_MAC_SIZE);
    entry->port = port;
    entry->last_seen = now;
    entry->next = bucket->head;
    bucket->head = entry;

    pthread_mutex_unlock(&bucket->lock);
    return 0;
}
static int entry_expired(const struct timespec *last_seen,
                         const struct timespec *now,
                         unsigned int max_age_seconds)
{
    time_t seconds = now->tv_sec - last_seen->tv_sec;

    if (now->tv_nsec < last_seen->tv_nsec) {
        seconds--;
    }

    return seconds >= 0 &&
           (uintmax_t)seconds >= (uintmax_t)max_age_seconds;
}

int mac_table_expire(MacTable *table, unsigned int max_age_seconds)
{
    if (table == NULL || max_age_seconds == 0) {
        return -1;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return -1;
    }

    int removed = 0;

    for (size_t i = 0; i < MAC_TABLE_BUCKETS; i++) {
        MacBucket *bucket = &table->buckets[i];

        if (pthread_mutex_lock(&bucket->lock) != 0) {
            return -1;
        }

        MacEntry **link = &bucket->head;
        while (*link != NULL) {
            MacEntry *entry = *link;

            if (entry_expired(&entry->last_seen, &now, max_age_seconds)) {
                *link = entry->next;
                free(entry);
                removed++;
            } else {
                link = &entry->next;
            }
        }

        if (pthread_mutex_unlock(&bucket->lock) != 0) {
            return -1;
        }
    }

    return removed;
}