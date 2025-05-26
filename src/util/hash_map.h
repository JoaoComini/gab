#ifndef GAB_HASH_MAP_H
#define GAB_HASH_MAP_H

#include <stdbool.h>
#include <stdlib.h>

#define GAB_HASH_MAP(Name, Alias, KeyType, ValueType)                                                        \
    typedef struct Name##Entry {                                                                             \
        KeyType key;                                                                                         \
        size_t hash;                                                                                         \
        ValueType value;                                                                                     \
        struct Name##Entry *next;                                                                            \
    } Name##Entry;                                                                                           \
                                                                                                             \
    typedef struct {                                                                                         \
        Name##Entry **buckets;                                                                               \
        size_t capacity;                                                                                     \
        size_t size;                                                                                         \
    } Name;                                                                                                  \
                                                                                                             \
    static void Alias##_init(Name *map, size_t capacity) {                                                   \
        map->buckets = calloc(capacity, sizeof(Name##Entry *));                                              \
        map->capacity = capacity;                                                                            \
        map->size = 0;                                                                                       \
    }                                                                                                        \
                                                                                                             \
    static Name *Alias##_create(size_t capacity) {                                                           \
        Name *map = malloc(sizeof(Name));                                                                    \
        Alias##_init(map, capacity);                                                                         \
        return map;                                                                                          \
    }                                                                                                        \
                                                                                                             \
    static void Alias##_resize(Name *map) {                                                                  \
        size_t new_cap = map->capacity * 2;                                                                  \
        Name##Entry **new_buckets = calloc(new_cap, sizeof(Name##Entry *));                                  \
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
        free(map->buckets);                                                                                  \
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
        Name##Entry *new_entry = malloc(sizeof(Name##Entry));                                                \
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
                Alias##_entry_free(entry->key, entry->value);                                                \
                free(entry);                                                                                 \
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
                Alias##_entry_free(entry->key, entry->value);                                                \
                free(entry);                                                                                 \
                entry = next;                                                                                \
            }                                                                                                \
        }                                                                                                    \
        free(map->buckets);                                                                                  \
    }                                                                                                        \
                                                                                                             \
    static void Alias##_destroy(Name *map) {                                                                 \
        Alias##_free(map);                                                                                   \
        free(map);                                                                                           \
    }

#endif
