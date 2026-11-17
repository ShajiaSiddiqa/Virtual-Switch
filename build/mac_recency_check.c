#include "mac_table.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Test helper. This test runs in one thread and does no concurrent mutation. */
static const MacEntry *find_for_test(
    const MacTable *table, const uint8_t mac[FRAME_MAC_SIZE])
{
    for (size_t i = 0; i < MAC_TABLE_BUCKETS; i++) {
        for (const MacEntry *entry = table->buckets[i].head;
             entry != NULL; entry = entry->next) {
            if (memcmp(entry->mac, mac, FRAME_MAC_SIZE) == 0) {
                return entry;
            }
        }
    }

    return NULL;
}

int main(void)
{
    MacTable table;
    const uint8_t a[FRAME_MAC_SIZE] = {0x02, 0, 0, 0, 0, 1};
    const uint8_t b[FRAME_MAC_SIZE] = {0x02, 0, 0, 0, 0, 2};
    const uint8_t unknown[FRAME_MAC_SIZE] = {0x02, 0, 0, 0, 0, 3};
    int port = -1;

    assert(mac_table_init(&table) == 0);
    assert(mac_table_insert(&table, a, 1) == 0);
    assert(mac_table_insert(&table, b, 2) == 0);

    const MacEntry *entry_a = find_for_test(&table, a);
    const MacEntry *entry_b = find_for_test(&table, b);
    assert(entry_a != NULL && entry_b != NULL);
    assert(entry_a->last_used < entry_b->last_used);

    struct timespec original_seen = entry_a->last_seen;
    uint_fast64_t sequence_before_miss = atomic_load(&table.use_sequence);

    assert(mac_table_lookup(&table, unknown, &port) == 0);
    assert(atomic_load(&table.use_sequence) == sequence_before_miss);

    assert(mac_table_lookup(&table, a, &port) == 1 && port == 1);
    assert(entry_a->last_used > entry_b->last_used);
    assert(entry_a->last_seen.tv_sec == original_seen.tv_sec);
    assert(entry_a->last_seen.tv_nsec == original_seen.tv_nsec);

    assert(mac_table_insert(&table, b, 2) == 0);
    assert(entry_b->last_used > entry_a->last_used);

    assert(mac_table_destroy(&table) == 0);
    puts("PASS: learning and lookup update LRU order without changing lookup aging");
    return 0;
}
