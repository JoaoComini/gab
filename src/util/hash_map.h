#ifndef GAB_HASH_MAP_H
#define GAB_HASH_MAP_H

#include "allocator.h"
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef enum HashMapState {
    HASH_MAP_EMPTY = 0,
    HASH_MAP_LIVE,
    HASH_MAP_DEAD,
} HashMapState;

#define GAB_HASH_MAP(Name, Alias, KeyType, ValueType)                                                        \
    typedef struct Name##Entry {                                                                             \
        KeyType key;                                                                                         \
        size_t hash;                                                                                         \
        ValueType value;                                                                                     \
        unsigned char state;                                                                                 \
    } Name##Entry;                                                                                           \
                                                                                                             \
    typedef struct Name {                                                                                    \
        Name##Entry *entries;                                                                                \
        size_t capacity;                                                                                     \
        size_t size;                                                                                         \
        /* Live plus tombstoned, so probes stay bounded as deletes accumulate. */                            \
        size_t occupied;                                                                                     \
        Allocator allocator;                                                                                 \
    } Name;                                                                                                  \
                                                                                                             \
    static void Alias##_init_alloc(Name *map, Allocator allocator, size_t capacity) {                        \
        assert(capacity > 0 && (capacity & (capacity - 1)) == 0 &&                                           \
               "probing masks, so capacity is a power of two");                                              \
        map->entries = allocator.alloc(allocator.ctx, capacity * sizeof(Name##Entry));                       \
        memset(map->entries, 0, capacity * sizeof(Name##Entry));                                             \
        map->capacity = capacity;                                                                            \
        map->size = 0;                                                                                       \
        map->occupied = 0;                                                                                   \
        map->allocator = allocator;                                                                          \
    }                                                                                                        \
                                                                                                             \
    static Name *Alias##_create_alloc(Allocator allocator, size_t capacity) {                                \
        Name *map = allocator.alloc(allocator.ctx, sizeof(Name));                                            \
        Alias##_init_alloc(map, allocator, capacity);                                                        \
        return map;                                                                                          \
    }                                                                                                        \
                                                                                                             \
    static void Alias##_init(Name *map, size_t capacity) {                                                   \
        Alias##_init_alloc(map, DEFAULT_ALLOCATOR, capacity);                                                \
    }                                                                                                        \
                                                                                                             \
    static Name *Alias##_create(size_t capacity) {                                                           \
        return Alias##_create_alloc(DEFAULT_ALLOCATOR, capacity);                                            \
    }                                                                                                        \
                                                                                                             \
    /* The slot holding the key, else the first free slot it may be inserted into. */                        \
    static Name##Entry *Alias##_probe(Name##Entry *entries, size_t capacity, KeyType key, size_t hash) {     \
        size_t idx = hash & (capacity - 1);                                                                  \
        Name##Entry *tombstone = NULL;                                                                       \
                                                                                                             \
        for (;;) {                                                                                           \
            Name##Entry *entry = &entries[idx];                                                              \
                                                                                                             \
            if (entry->state == HASH_MAP_EMPTY) {                                                            \
                return tombstone ? tombstone : entry;                                                        \
            }                                                                                                \
            if (entry->state == HASH_MAP_DEAD) {                                                             \
                if (!tombstone) {                                                                            \
                    tombstone = entry;                                                                       \
                }                                                                                            \
            } else if (entry->hash == hash && Alias##_key_equals(key, entry->key)) {                         \
                return entry;                                                                                \
            }                                                                                                \
                                                                                                             \
            idx = (idx + 1) & (capacity - 1);                                                                \
        }                                                                                                    \
    }                                                                                                        \
                                                                                                             \
    /* Tombstones alone do not justify a bigger table, so a mostly-dead one is rehashed at its size. */      \
    static void Alias##_resize(Name *map) {                                                                  \
        size_t new_cap = map->size >= map->capacity * 0.25 ? map->capacity * 2 : map->capacity;              \
        Name##Entry *new_entries = map->allocator.alloc(map->allocator.ctx, new_cap * sizeof(Name##Entry));  \
        memset(new_entries, 0, new_cap * sizeof(Name##Entry));                                               \
                                                                                                             \
        for (size_t i = 0; i < map->capacity; i++) {                                                         \
            Name##Entry *entry = &map->entries[i];                                                           \
            if (entry->state != HASH_MAP_LIVE) {                                                             \
                continue;                                                                                    \
            }                                                                                                \
            *Alias##_probe(new_entries, new_cap, entry->key, entry->hash) = *entry;                          \
        }                                                                                                    \
                                                                                                             \
        map->allocator.free(map->allocator.ctx, map->entries, map->capacity * sizeof(Name##Entry));          \
        map->entries = new_entries;                                                                          \
        map->capacity = new_cap;                                                                             \
        map->occupied = map->size;                                                                           \
    }                                                                                                        \
                                                                                                             \
    static ValueType *Alias##_insert(Name *map, KeyType key, ValueType value) {                              \
        if (map->occupied >= map->capacity * 0.5) {                                                          \
            Alias##_resize(map);                                                                             \
        }                                                                                                    \
                                                                                                             \
        size_t hash = Alias##_hash(key);                                                                     \
        Name##Entry *entry = Alias##_probe(map->entries, map->capacity, key, hash);                          \
                                                                                                             \
        if (entry->state == HASH_MAP_LIVE) {                                                                 \
            return NULL;                                                                                     \
        }                                                                                                    \
                                                                                                             \
        if (entry->state == HASH_MAP_EMPTY) {                                                                \
            map->occupied++;                                                                                 \
        }                                                                                                    \
                                                                                                             \
        entry->key = key;                                                                                    \
        entry->hash = hash;                                                                                  \
        entry->value = value;                                                                                \
        entry->state = HASH_MAP_LIVE;                                                                        \
        map->size++;                                                                                         \
        return &entry->value;                                                                                \
    }                                                                                                        \
                                                                                                             \
    static ValueType *Alias##_lookup(Name *map, KeyType key) {                                               \
        Name##Entry *entry = Alias##_probe(map->entries, map->capacity, key, Alias##_hash(key));             \
                                                                                                             \
        return entry->state == HASH_MAP_LIVE ? &entry->value : NULL;                                         \
    }                                                                                                        \
                                                                                                             \
    static bool Alias##_delete(Name *map, KeyType key) {                                                     \
        if (map->size == 0) {                                                                                \
            return false;                                                                                    \
        }                                                                                                    \
                                                                                                             \
        Name##Entry *entry = Alias##_probe(map->entries, map->capacity, key, Alias##_hash(key));             \
                                                                                                             \
        if (entry->state != HASH_MAP_LIVE) {                                                                 \
            return false;                                                                                    \
        }                                                                                                    \
                                                                                                             \
        entry->state = HASH_MAP_DEAD;                                                                        \
        map->size--;                                                                                         \
        return true;                                                                                         \
    }                                                                                                        \
                                                                                                             \
    static void Alias##_free(Name *map) {                                                                    \
        map->allocator.free(map->allocator.ctx, map->entries, map->capacity * sizeof(Name##Entry));          \
    }                                                                                                        \
                                                                                                             \
    static void Alias##_destroy(Name *map) {                                                                 \
        Alias##_free(map);                                                                                   \
        map->allocator.free(map->allocator.ctx, map, sizeof(Name));                                          \
    }

#define GAB_HASH_MAP_FOR_EACH(map, entry)                                                                    \
    for (size_t _i = 0; _i < (map)->capacity; _i++)                                                          \
        for (typeof((map)->entries) entry = &(map)->entries[_i]; entry && entry->state == HASH_MAP_LIVE;     \
             entry = NULL)

#endif
