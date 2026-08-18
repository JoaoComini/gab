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
    /* Removes by moving the last item into the hole, so removal is O(1) and    */                           \
    /* order is not preserved. The removed item is returned rather than freed:  */                           \
    /* this hands ownership back to the caller, unlike _free and _emplace which */                           \
    /* dispose of what they displace.                                           */                           \
    /*                                                                          */                           \
    /* A caller keeping an index into the list has to fix up the moved item,    */                           \
    /* which is why the index it moved from is reported through 'moved_from'.   */                           \
    /* It is set to the removed index when nothing moved — removing the last    */                           \
    /* item — so a caller can compare the two rather than special-case it.      */                           \
    static inline ItemType Alias##_swap_remove(Name *list, size_t index, size_t *moved_from) {               \
        assert(index < list->size);                                                                          \
                                                                                                             \
        ItemType removed = list->data[index];                                                                \
        size_t last = list->size - 1;                                                                        \
                                                                                                             \
        list->data[index] = list->data[last];                                                                \
        list->size = last;                                                                                   \
                                                                                                             \
        if (moved_from) {                                                                                    \
            *moved_from = last;                                                                              \
        }                                                                                                    \
                                                                                                             \
        return removed;                                                                                      \
    }                                                                                                        \
                                                                                                             \
    static inline void Alias##_free(Name *list) {                                                            \
        for (size_t i = 0; i < list->size; i++) {                                                            \
            Alias##_item_free(list->data[i]);                                                                \
        }                                                                                                    \
        free(list->data);                                                                                    \
    }

#endif
