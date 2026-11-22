#define _POSIX_C_SOURCE 200809L
#include "mac_table.h"

#include <stdio.h>  
#include <stdlib.h>  
#include <string.h>  
#include <errno.h>

static size_t bucket_index(const uint8_t mac[FRAME_MAC_SIZE], uint16_t vlan)
{
    unsigned int hash = vlan;

    for (size_t i = 0; i < FRAME_MAC_SIZE; i++) 
    {
        hash = hash * 31u + mac[i];
    }

    return hash % MAC_TABLE_BUCKETS;
}

// caller hold bucket`s lock. 
static MacEntry *find_entry(MacBucket *bucket, const uint8_t mac[FRAME_MAC_SIZE], uint16_t vlan)
{
    for (MacEntry *entry = bucket->head; entry != NULL; entry = entry->next) 
    {
        if (entry->vlan == vlan && memcmp(entry->mac, mac, FRAME_MAC_SIZE) == 0) 
            return entry;
    }

    return NULL;
}

int mac_table_init(MacTable *table)
{
    if (table == NULL) 
        return -1;

    table->checkpoint = NULL;
    table->checkpoint_context = NULL;
    table->move_logger = NULL;
    table->move_log_context = NULL;
    atomic_init(&table->use_sequence, 0);

    table->entry_count = 0;
    if (pthread_mutex_init(&table->capacity_lock, NULL) != 0) 
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
            pthread_mutex_destroy(&table->capacity_lock);
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

    if (pthread_mutex_destroy(&table->capacity_lock) != 0) 
    result = -1;

    return result;
}

static void mark_used(MacTable *table, MacEntry *entry)
{
    entry->last_used = atomic_fetch_add_explicit(&table->use_sequence, 1,memory_order_relaxed) + 1;
}

/* Caller holds capacity_lock and no bucket locks. */
static int checkpoint_locked(MacTable *table)
{
    if (table->checkpoint == NULL)
        return 0;
    size_t locked = 0;
    while (locked < MAC_TABLE_BUCKETS) {
        int error = pthread_mutex_lock(&table->buckets[locked].lock);
        if (error != 0) {
            while (locked > 0)
                pthread_mutex_unlock(&table->buckets[--locked].lock);
            errno = error;
            return -1;
        }
        locked++;
    }
    MacStoreImage image = {0};
    int result = 0;
    for (size_t i = 0; i < MAC_TABLE_BUCKETS && result == 0; i++) {
        for (MacEntry *entry = table->buckets[i].head; entry; entry = entry->next) {
            if (image.count == MAC_STORE_CAPACITY) {errno = ENOSPC; result = -1; break;}
            MacStoreEntry *record = &image.entries[image.count++];
            memcpy(record->mac, entry->mac, FRAME_MAC_SIZE);
            record->vlan = entry->vlan;
            record->port = entry->port;
            record->last_seen_seconds = (int64_t)entry->last_seen.tv_sec;
            record->last_seen_nanoseconds = (int32_t)entry->last_seen.tv_nsec;
            record->last_used = entry->last_used;
        }
    }
    image.use_sequence = atomic_load_explicit(&table->use_sequence, memory_order_relaxed);
    if (result == 0)
        result = table->checkpoint(table->checkpoint_context, &image);
    int saved = errno;
    while (locked > 0)
        if (pthread_mutex_unlock(&table->buckets[--locked].lock) != 0)
            abort();
    errno = saved;
    return result;
}

int mac_table_checkpoint(MacTable *table)
{
    if (table == NULL) {errno = EINVAL; return -1;}
    if (table->checkpoint == NULL) return 0;
    int error = pthread_mutex_lock(&table->capacity_lock);
    if (error != 0) {errno = error; return -1;}
    int result = checkpoint_locked(table);
    int saved = errno;
    if (pthread_mutex_unlock(&table->capacity_lock) != 0) abort();
    errno = saved;
    return result;
}

int mac_table_lookup_vlan(MacTable *table, const uint8_t mac[FRAME_MAC_SIZE], uint16_t vlan, int *port)
{
    if (table == NULL || mac == NULL || port == NULL || vlan < 1 || vlan > 4094) 
        return -1;

    MacBucket *bucket = &table->buckets[bucket_index(mac, vlan)];
    if (pthread_mutex_lock(&bucket->lock) != 0) 
        return -1;

    MacEntry *entry = find_entry(bucket, mac, vlan);
    int found = 0;
    int found_port = 0;

    if (entry != NULL && !entry->recovered)
    {
        found_port = entry->port;
        mark_used(table, entry);
        found = 1;
    }

    pthread_mutex_unlock(&bucket->lock);
    /* Release the stripe before taking the capacity/snapshot locks. */
    if (found && mac_table_checkpoint(table) != 0)
        return -1;
    if (found) *port = found_port;
    return found;
}

static int replace_oldest(MacTable *table, size_t target_index,  MacEntry *replacement)
{
    size_t locked = 0;

    while (locked < MAC_TABLE_BUCKETS) {
        if (pthread_mutex_lock(&table->buckets[locked].lock) != 0) {
            while (locked > 0) {
                pthread_mutex_unlock(&table->buckets[--locked].lock);
            }
            return -1;
        }
        locked++;
    }

    MacEntry **oldest_link = NULL;

    for (size_t i = 0; i < MAC_TABLE_BUCKETS; i++) {
        for (MacEntry **link = &table->buckets[i].head;
             *link != NULL; link = &(*link)->next) {
            if (oldest_link == NULL ||
                (*link)->last_used < (*oldest_link)->last_used) {
                oldest_link = link;
            }
        }
    }

    int result = -1;
    if (oldest_link != NULL) {
        MacEntry *oldest = *oldest_link;
        *oldest_link = oldest->next;
        free(oldest);

        MacBucket *target = &table->buckets[target_index];
        mark_used(table, replacement);
        replacement->next = target->head;
        target->head = replacement;
        result = 0;
    }

    while (locked > 0) {
        pthread_mutex_unlock(&table->buckets[--locked].lock);
    }

    return result;
}

static size_t count_port_entries(const MacTable *table, int port)
{
    size_t count = 0;

    for (size_t i = 0; i < MAC_TABLE_BUCKETS; i++) {
        for (const MacEntry *entry = table->buckets[i].head;
             entry != NULL; entry = entry->next) {
            if (entry->port == port && !entry->recovered) {
                count++;
            }
        }
    }

    return count;
}

int mac_table_insert_vlan(MacTable *table, const uint8_t mac[FRAME_MAC_SIZE], uint16_t vlan, int port)
{
    if (table == NULL || mac == NULL || port < 0 || vlan < 1 || vlan > 4094) {
        return -1;
    }

    if (pthread_mutex_lock(&table->capacity_lock) != 0) {
        return -1;
    }

    size_t index = bucket_index(mac, vlan);
    MacBucket *bucket = &table->buckets[index];

    if (pthread_mutex_lock(&bucket->lock) != 0) {
        pthread_mutex_unlock(&table->capacity_lock);
        return -1;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        pthread_mutex_unlock(&bucket->lock);
        pthread_mutex_unlock(&table->capacity_lock);
        return -1;
    }

    MacEntry *entry = find_entry(bucket, mac, vlan);
    if ((entry == NULL || entry->port != port) && count_port_entries(table, port) >= MAC_TABLE_PORT_LIMIT) {
        pthread_mutex_unlock(&bucket->lock);
        pthread_mutex_unlock(&table->capacity_lock);
        return MAC_TABLE_PORT_LIMIT_REACHED;
    }

    if (entry != NULL) {
        int old_port = entry->port;
        entry->port = port;
        entry->recovered = 0;
        entry->last_seen = now;
        mark_used(table, entry);

        pthread_mutex_unlock(&bucket->lock);
        int committed = checkpoint_locked(table);
        pthread_mutex_unlock(&table->capacity_lock);
        if (committed != 0) return -1;

        if (old_port != port) {
            if (table->move_logger != NULL)
                table->move_logger(table->move_log_context, mac, vlan, old_port, port);
            else
                printf("MAC moved: port %d -> port %d\n", old_port, port);
        }
        return 0;
    }

    entry = malloc(sizeof(*entry));
    if (entry == NULL) {
        pthread_mutex_unlock(&bucket->lock);
        pthread_mutex_unlock(&table->capacity_lock);
        return -1;
    }

    memcpy(entry->mac, mac, FRAME_MAC_SIZE);
    entry->vlan = vlan;
    entry->port = port;
    entry->recovered = 0;
    entry->last_seen = now;

    if (table->entry_count < MAC_TABLE_CAPACITY) {
        mark_used(table, entry);
        entry->next = bucket->head;
        bucket->head = entry;
        table->entry_count++;

        pthread_mutex_unlock(&bucket->lock);
        int committed = checkpoint_locked(table);
        pthread_mutex_unlock(&table->capacity_lock);
        return committed;
    }

    pthread_mutex_unlock(&bucket->lock);
    int result = replace_oldest(table, index, entry);
    if (result == 0) {
        /* Table owns entry after replacement, even if publication fails. */
        result = checkpoint_locked(table);
    } else {
        free(entry);
    }
    pthread_mutex_unlock(&table->capacity_lock);
    return result;
}

static int entry_expired(const struct timespec *last_seen,const struct timespec *now, unsigned int max_age_seconds)
{
    time_t seconds = now->tv_sec - last_seen->tv_sec;

    if (now->tv_nsec < last_seen->tv_nsec) {
        seconds--;
    }

    return seconds >= 0 &&
           (uintmax_t)seconds >= (uintmax_t)max_age_seconds;
}

static int expire_entries(MacTable *table, unsigned int max_age_seconds){
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
                table->entry_count--;
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

int mac_table_expire(MacTable *table, unsigned int max_age_seconds)
{
    if (table == NULL || max_age_seconds == 0) {
        return -1;
    }

    if (pthread_mutex_lock(&table->capacity_lock) != 0) {
        return -1;
    }

    int removed = expire_entries(table, max_age_seconds);
    if (removed > 0 && checkpoint_locked(table) != 0)
        removed = -1;

    if (pthread_mutex_unlock(&table->capacity_lock) != 0) {
        return -1;
    }

    return removed;
}

int mac_table_remove_port(MacTable *table, int port)
{
    if (table == NULL || port < 0 ||
        pthread_mutex_lock(&table->capacity_lock) != 0) {
        return -1;
    }

    int removed = 0;

    for (size_t i = 0; i < MAC_TABLE_BUCKETS; i++) {
        MacBucket *bucket = &table->buckets[i];

        if (pthread_mutex_lock(&bucket->lock) != 0) {
            removed = -1;
            break;
        }

        MacEntry **link = &bucket->head;
        while (*link != NULL) {
            MacEntry *entry = *link;

            if (entry->port == port && !entry->recovered) {
                *link = entry->next;
                free(entry);
                table->entry_count--;
                removed++;
            } else {
                link = &entry->next;
            }
        }

        if (pthread_mutex_unlock(&bucket->lock) != 0) {
            removed = -1;
            break;
        }
    }

    if (removed > 0 && checkpoint_locked(table) != 0)
        removed = -1;
    if (pthread_mutex_unlock(&table->capacity_lock) != 0) {
        return -1;
    }

    return removed;
}

static int unlock_table(MacTable *table)
{
    int first_error = 0;

    for (size_t i = MAC_TABLE_BUCKETS; i > 0; i--) {
        int error = pthread_mutex_unlock(&table->buckets[i - 1].lock);
        if (first_error == 0) {
            first_error = error;
        }
    }

    int error = pthread_mutex_unlock(&table->capacity_lock);
    if (first_error == 0) 
        first_error = error;

    if (first_error != 0) {
        errno = first_error;
        return -1;
    }

    return 0;
}

static int lock_table(MacTable *table)
{
    int error = pthread_mutex_lock(&table->capacity_lock);
    if (error != 0) {
        errno = error;
        return -1;
    }

    size_t locked = 0;

    while (locked < MAC_TABLE_BUCKETS) {
        error = pthread_mutex_lock(&table->buckets[locked].lock);

        if (error != 0) {
            while (locked > 0) {
                int cleanup = pthread_mutex_unlock(
                    &table->buckets[--locked].lock);
                if (cleanup != 0) {
                    abort();
                }
            }

            int cleanup = pthread_mutex_unlock(&table->capacity_lock);
            if (cleanup != 0) 
                abort();

            errno = error;
            return -1;
        }

        locked++;
    }

    return 0;
}

int mac_table_snapshot(MacTable *table, MacRecord *records, size_t capacity, size_t *count)
{
    if (table == NULL || records == NULL || count == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (lock_table(table) == -1) 
        return -1;

    if (capacity < table->entry_count) {
        if (unlock_table(table) == -1) 
            return -1;

        errno = ENOSPC;
        return -1;
    }

    size_t used = 0;

    for (size_t i = 0; i < MAC_TABLE_BUCKETS; i++) {
        for (MacEntry *entry = table->buckets[i].head;  entry != NULL; entry = entry->next) {
            memcpy(records[used].mac, entry->mac, FRAME_MAC_SIZE);
            records[used].port = entry->port;
            records[used].vlan = entry->vlan;
            used++;
        }
    }

    *count = used;
    return unlock_table(table);
}

int mac_table_clear(MacTable *table)
{
    if (table == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (lock_table(table) == -1) 
        return -1;

    for (size_t i = 0; i < MAC_TABLE_BUCKETS; i++) {
        MacEntry *entry = table->buckets[i].head;
        table->buckets[i].head = NULL;

        while (entry != NULL) {
            MacEntry *next = entry->next;
            free(entry);
            entry = next;
        }
    }

    table->entry_count = 0;
    for (size_t i = MAC_TABLE_BUCKETS; i > 0; i--)
        if (pthread_mutex_unlock(&table->buckets[i - 1].lock) != 0) abort();
    int result = checkpoint_locked(table);
    int saved = errno;
    if (pthread_mutex_unlock(&table->capacity_lock) != 0) abort();
    errno = saved;
    return result;
}
/* Existing callers retain VLAN 1 behavior. */
int mac_table_lookup(MacTable *table, const uint8_t mac[FRAME_MAC_SIZE], int *port)
{
    return mac_table_lookup_vlan(table, mac, 1, port);
}

int mac_table_insert(MacTable *table, const uint8_t mac[FRAME_MAC_SIZE], int port)
{
    return mac_table_insert_vlan(table, mac, 1, port);
}

void mac_table_set_move_logger(MacTable *table, MacMoveLogger callback, void *context)
{
    table->move_logger = callback;
    table->move_log_context = context;
}

void mac_table_set_checkpoint(MacTable *table, MacCheckpoint callback, void *context)
{
    table->checkpoint = callback;
    table->checkpoint_context = context;
}

int mac_table_restore(MacTable *table, const MacStoreImage *image)
{
    if (table == NULL || image == NULL || image->count > MAC_STORE_CAPACITY ||
        image->count > MAC_TABLE_CAPACITY) {errno = EINVAL; return -1;}
    MacEntry *heads[MAC_TABLE_BUCKETS] = {0};
    int result = -1;
    for (size_t i = 0; i < image->count; i++) {
        const MacStoreEntry *record = &image->entries[i];
        if (record->vlan < 1 || record->vlan > 4094 || record->port < 0 ||
            (uint32_t)record->port >= MAC_STORE_MAX_PORTS || record->last_seen_seconds < 0 ||
            (int64_t)(time_t)record->last_seen_seconds != record->last_seen_seconds ||
            record->last_seen_nanoseconds < 0 || record->last_seen_nanoseconds >= 1000000000 ||
            record->last_used > image->use_sequence) {errno = EINVAL; goto cleanup;}
        size_t bucket = bucket_index(record->mac, record->vlan);
        for (MacEntry *entry = heads[bucket]; entry; entry = entry->next)
            if (entry->vlan == record->vlan && memcmp(entry->mac, record->mac, FRAME_MAC_SIZE) == 0) {
                errno = EINVAL; goto cleanup;
            }
        MacEntry *entry = malloc(sizeof(*entry));
        if (entry == NULL) goto cleanup;
        memcpy(entry->mac, record->mac, FRAME_MAC_SIZE);
        entry->vlan = record->vlan;
        entry->port = record->port;
        entry->recovered = 1;
        entry->last_seen.tv_sec = (time_t)record->last_seen_seconds;
        entry->last_seen.tv_nsec = record->last_seen_nanoseconds;
        entry->last_used = record->last_used;
        entry->next = heads[bucket];
        heads[bucket] = entry;
    }
    if (lock_table(table) != 0) goto cleanup;
    if (table->entry_count != 0 || table->checkpoint != NULL) {
        unlock_table(table);
        errno = EBUSY;
        goto cleanup;
    }
    for (size_t i = 0; i < MAC_TABLE_BUCKETS; i++) {
        table->buckets[i].head = heads[i];
        heads[i] = NULL;
    }
    table->entry_count = image->count;
    atomic_store_explicit(&table->use_sequence, image->use_sequence, memory_order_relaxed);
    result = unlock_table(table);
cleanup:
    {
        int saved = errno;
        for (size_t i = 0; i < MAC_TABLE_BUCKETS; i++) {
            while (heads[i]) {
                MacEntry *entry = heads[i]; heads[i] = entry->next; free(entry);
            }
        }
        errno = saved;
        return result;
    }
}
