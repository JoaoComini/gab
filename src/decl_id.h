#ifndef GAB_DECL_ID_H
#define GAB_DECL_ID_H

#include "string/string.h"

#include <stdbool.h>
#include <stddef.h>

/* Which declaration this is, derived from source alone, so every compilation that reads it agrees.
 * The names are interned in the compilation's pool, so an id compares as a pointer triple. */
/* The module the compiler's own declarations are found in, which an id naming one must match. */
#define GAB_CORE_MODULE "core"

typedef struct DeclId {
    /* Null for an intrinsic, which no module qualifies. */
    const String *module;

    /* Null for a type or a free function. */
    const String *owner;

    const String *name;

    /* Which impl block, once syntax lets one type declare two functions of a name; always 0 until then. */
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
