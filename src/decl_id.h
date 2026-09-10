#ifndef GAB_DECL_ID_H
#define GAB_DECL_ID_H

#include "string/string.h"

#include <stdbool.h>
#include <stddef.h>

#define GAB_CORE_MODULE "core"

typedef struct DeclId {
    const String *module;

    const String *owner;

    const String *name;

    size_t block;
} DeclId;

static inline bool decl_id_equals(DeclId id, DeclId other) {
    return id.module == other.module && id.owner == other.owner && id.name == other.name &&
           id.block == other.block;
}

static inline size_t decl_id_hash(DeclId id) {
    size_t hash = (size_t)id.module;

    hash = hash * 31 + (size_t)id.owner;
    hash = hash * 31 + (size_t)id.name;
    hash = hash * 31 + id.block;

    return hash;
}

static inline bool decl_id_is_set(DeclId id) { return id.name != NULL; }

#endif
