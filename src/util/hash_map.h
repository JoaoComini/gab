#ifndef GAB_HASH_MAP_H
#define GAB_HASH_MAP_H

#include "allocator.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define GAB_HASH_MAP(Name, Alias, KeyType, ValueType)                                                        \
    typedef struct Name##Entry {                                                                             \
        KeyType key;                                                                                         \
        size_t hash;                                                                                         \
        ValueType value;                                                                                     \
        struct Name##Entry *next;                                                                            \
    } Name##Entry;                                                                                           \
                                                                                                             \
    typedef struct Name {                                                                                    \
        Name##Entry **buckets;                                                                               \
        size_t capacity;                                                                                     \
        size_t size;                                                                                         \
        Allocator allocator;                                                                                 \
    } Name;                                                                                                  \
                                                                                                             \
    static void Alias##_init_alloc(Name *map, Allocator allocator, size_t capacity) {                        \
        map->buckets = allocator.alloc(allocator.ctx, capacity * sizeof(Name##Entry *));                     \
        memset(map->buckets, 0, capacity * sizeof(Name##Entry *));                                           \
        map->capacity = capacity;                                                                            \
        map->size = 0;                                                                                       \
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
    static void Alias##_resize(Name *map) {                                                                  \
        size_t new_cap = map->capacity * 2;                                                                  \
        Name##Entry **new_buckets =                                                                          \
            map->allocator.alloc(map->allocator.ctx, new_cap * sizeof(Name##Entry *));                       \
        memset(new_buckets, 0, new_cap * sizeof(Name##Entry *));                                             \
                                                                                                             \
        for (size_t i = 0; i < map->capacity; i++) {                                                         \
            Name##Entry *entry = map->buckets[i];                                                            \
            while (entry) {                                                                                  \
                Name##Entry *next = entry->next;                                                             \
                size_t idx = entry->hash % new_cap;                                                          \
                entry->next = new_buckets[idx];                                                              \
                new_buckets[idx] = entry;                                                                    \
                entry = next;                                                                                \
            }                                                                                                \
        }                                                                                                    \
                                                                                                             \
        map->allocator.free(map->allocator.ctx, map->buckets);                                               \
        map->buckets = new_buckets;                                                                          \
        map->capacity = new_cap;                                                                             \
    }                                                                                                        \
                                                                                                             \
    static ValueType *Alias##_insert(Name *map, KeyType key, ValueType value) {                              \
        if (map->size >= map->capacity * 0.5) {                                                              \
            Alias##_resize(map);                                                                             \
        }                                                                                                    \
                                                                                                             \
        size_t hash = Alias##_hash(key);                                                                     \
        size_t idx = hash % map->capacity;                                                                   \
        Name##Entry *entry = map->buckets[idx];                                                              \
                                                                                                             \
        while (entry) {                                                                                      \
            if (Alias##_key_equals(key, entry->key)) {                                                       \
                return NULL;                                                                                 \
            }                                                                                                \
            entry = entry->next;                                                                             \
        }                                                                                                    \
                                                                                                             \
        Name##Entry *new_entry = map->allocator.alloc(map->allocator.ctx, sizeof(Name##Entry));              \
        new_entry->key = Alias##_key_dup(key);                                                               \
        new_entry->hash = hash;                                                                              \
        new_entry->value = value;                                                                            \
        new_entry->next = map->buckets[idx];                                                                 \
        map->buckets[idx] = new_entry;                                                                       \
        map->size++;                                                                                         \
        return &map->buckets[idx]->value;                                                                    \
    }                                                                                                        \
                                                                                                             \
    static ValueType *Alias##_lookup(Name *map, KeyType key) {                                               \
        size_t idx = Alias##_hash(key) % map->capacity;                                                      \
        Name##Entry *entry = map->buckets[idx];                                                              \
                                                                                                             \
        while (entry) {                                                                                      \
            if (Alias##_key_equals(key, entry->key)) {                                                       \
                return &entry->value;                                                                        \
            }                                                                                                \
            entry = entry->next;                                                                             \
        }                                                                                                    \
        return NULL;                                                                                         \
    }                                                                                                        \
                                                                                                             \
    static bool Alias##_delete(Name *map, KeyType key) {                                                     \
        if (map->size == 0)                                                                                  \
            return false;                                                                                    \
                                                                                                             \
        size_t idx = Alias##_hash(key) % map->capacity;                                                      \
        Name##Entry *entry = map->buckets[idx];                                                              \
        Name##Entry *prev = NULL;                                                                            \
                                                                                                             \
        while (entry) {                                                                                      \
            if (Alias##_key_equals(key, entry->key)) {                                                       \
                if (prev) {                                                                                  \
                    prev->next = entry->next;                                                                \
                } else {                                                                                     \
                    map->buckets[idx] = entry->next;                                                         \
                }                                                                                            \
                                                                                                             \
                map->allocator.free(map->allocator.ctx, entry);                                              \
                map->size--;                                                                                 \
                return true;                                                                                 \
            }                                                                                                \
                                                                                                             \
            prev = entry;                                                                                    \
            entry = entry->next;                                                                             \
        }                                                                                                    \
        return false;                                                                                        \
    }                                                                                                        \
                                                                                                             \
    static void Alias##_free(Name *map) {                                                                    \
        for (size_t i = 0; i < map->capacity; i++) {                                                         \
            Name##Entry *entry = map->buckets[i];                                                            \
            while (entry) {                                                                                  \
                Name##Entry *next = entry->next;                                                             \
                map->allocator.free(map->allocator.ctx, entry);                                              \
                entry = next;                                                                                \
            }                                                                                                \
        }                                                                                                    \
        map->allocator.free(map->allocator.ctx, map->buckets);                                               \
    }                                                                                                        \
                                                                                                             \
    static void Alias##_destroy(Name *map) {                                                                 \
        Alias##_free(map);                                                                                   \
        map->allocator.free(map->allocator.ctx, map);                                                        \
    }

#endif
