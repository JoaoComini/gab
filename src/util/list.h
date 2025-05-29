#ifndef GAB_LIST_H
#define GAB_LIST_H

#include <assert.h>
#include <stdlib.h>

#define GAB_LIST(Name, Alias, ItemType)                                                                      \
    typedef struct Name {                                                                                    \
        ItemType *data;                                                                                      \
        size_t capacity;                                                                                     \
        size_t size;                                                                                         \
    } Name;                                                                                                  \
                                                                                                             \
    static inline Name Alias##_create() {                                                                    \
        return (Name){                                                                                       \
            .data = NULL,                                                                                    \
            .size = 0,                                                                                       \
            .capacity = 0,                                                                                   \
        };                                                                                                   \
    }                                                                                                        \
                                                                                                             \
    static inline void Alias##_grow(Name *list, size_t n) {                                                  \
        if (list->size + n <= list->capacity) {                                                              \
            return;                                                                                          \
        }                                                                                                    \
                                                                                                             \
        size_t capacity = list->capacity == 0 ? 1 : list->capacity * 2;                                      \
        while (capacity < list->size + n) {                                                                  \
            capacity *= 2;                                                                                   \
        }                                                                                                    \
                                                                                                             \
        list->data = realloc(list->data, capacity * sizeof(ItemType));                                       \
        list->capacity = capacity;                                                                           \
    }                                                                                                        \
                                                                                                             \
    static inline void Alias##_resize(Name *list, size_t n) {                                                \
        Alias##_grow(list, n - list->size);                                                                  \
        list->size = n;                                                                                      \
    }                                                                                                        \
                                                                                                             \
    static inline void Alias##_add(Name *list, ItemType item) {                                              \
        Alias##_grow(list, 1);                                                                               \
        list->data[list->size++] = item;                                                                     \
    }                                                                                                        \
                                                                                                             \
    static inline void Alias##_emplace(Name *list, size_t index, ItemType item) {                            \
        assert(index >= 0 && index < list->size);                                                            \
                                                                                                             \
        Alias##_item_free(list->data[index]);                                                                \
        list->data[index] = item;                                                                            \
    }                                                                                                        \
                                                                                                             \
    static inline ItemType Alias##_get(Name *list, size_t index) {                                           \
        assert(index >= 0 && index < list->size);                                                            \
                                                                                                             \
        return list->data[index];                                                                            \
    }                                                                                                        \
                                                                                                             \
    static inline ItemType Alias##_back(Name *list) { return list->data[list->size - 1]; }                   \
                                                                                                             \
    static inline void Alias##_free(Name *list) {                                                            \
        for (size_t i = 0; i < list->size; i++) {                                                            \
            Alias##_item_free(list->data[i]);                                                                \
        }                                                                                                    \
        free(list->data);                                                                                    \
    }

#endif
