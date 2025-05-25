#ifndef GAB_HASH_MAP_H
#define GAB_HASH_MAP_H

#include "string_ref.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static size_t djb2_hash(StringRef str) {
    size_t hash = 5381;

    for (int i = 0; i < str.length; i++) {
        int ch = str.data[i];
        hash = ((hash << 5) + hash) + ch;
    }

    return hash;
}

#define GAB_HASH_MAP(Name, Alias, ValueType)                                                                 \
    typedef struct Name##Entry {                                                                             \
        char *key;                                                                                           \
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
    static Name *Alias##_create(size_t capacity) {                                                           \
        Name *map = malloc(sizeof(Name));                                                                    \
        map->buckets = calloc(capacity, sizeof(Name##Entry *));                                              \
        map->capacity = capacity;                                                                            \
        map->size = 0;                                                                                       \
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
    static bool Alias##_insert(Name *map, StringRef key, ValueType value) {                                  \
        if (map->size >= map->capacity * 0.5) {                                                              \
            Alias##_resize(map);                                                                             \
        }                                                                                                    \
                                                                                                             \
        size_t hash = djb2_hash(key);                                                                        \
        size_t idx = hash % map->capacity;                                                                   \
        Name##Entry *entry = map->buckets[idx];                                                              \
                                                                                                             \
        while (entry) {                                                                                      \
            if (string_ref_equals_cstr(key, entry->key)) {                                                   \
                return false;                                                                                \
            }                                                                                                \
            entry = entry->next;                                                                             \
        }                                                                                                    \
                                                                                                             \
        Name##Entry *new_entry = malloc(sizeof(Name##Entry));                                                \
        new_entry->key = string_ref_to_cstr(key);                                                            \
        new_entry->hash = hash;                                                                              \
        new_entry->value = value;                                                                            \
        new_entry->next = map->buckets[idx];                                                                 \
        map->buckets[idx] = new_entry;                                                                       \
        map->size++;                                                                                         \
        return true;                                                                                         \
    }                                                                                                        \
                                                                                                             \
    static ValueType *Alias##_lookup(Name *map, StringRef key) {                                             \
        size_t idx = djb2_hash(key) % map->capacity;                                                         \
        Name##Entry *entry = map->buckets[idx];                                                              \
                                                                                                             \
        while (entry) {                                                                                      \
            if (string_ref_equals_cstr(key, entry->key)) {                                                   \
                return &entry->value;                                                                        \
            }                                                                                                \
            entry = entry->next;                                                                             \
        }                                                                                                    \
        return NULL;                                                                                         \
    }                                                                                                        \
                                                                                                             \
    static bool Alias##_delete(Name *map, StringRef key) {                                                   \
        if (map->size == 0)                                                                                  \
            return false;                                                                                    \
                                                                                                             \
        size_t idx = djb2_hash(key) % map->capacity;                                                         \
        Name##Entry *entry = map->buckets[idx];                                                              \
        Name##Entry *prev = NULL;                                                                            \
                                                                                                             \
        while (entry) {                                                                                      \
            if (string_ref_equals_cstr(key, entry->key)) {                                                   \
                if (prev) {                                                                                  \
                    prev->next = entry->next;                                                                \
                } else {                                                                                     \
                    map->buckets[idx] = entry->next;                                                         \
                }                                                                                            \
                                                                                                             \
                free(entry->key);                                                                            \
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
                free(entry->key);                                                                            \
                free(entry);                                                                                 \
                entry = next;                                                                                \
            }                                                                                                \
        }                                                                                                    \
        free(map->buckets);                                                                                  \
        free(map);                                                                                           \
    }

#endif
