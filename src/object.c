#include "object.h"

#include <assert.h>
#include <string.h>

ObjectHeader *object_of(void *payload) {
    assert(payload && "a header is only meaningful for a live object");

    return (ObjectHeader *)payload - 1;
}

void *object_alloc(Allocator allocator, const Type *type) {
    return object_alloc_sized(allocator, type, type ? type->size : 0, 1);
}

void *object_alloc_sized(Allocator allocator, const Type *type, size_t size, size_t count) {
    assert(type && "an object needs a type; freeing it walks its fields");

    ObjectHeader *header = allocator.alloc(allocator.ctx, sizeof(ObjectHeader) + size);

    if (!header) {
        return NULL;
    }

    header->type = type;
    header->count = count;

    // Zeroed so that a pointer field nobody assigned is NULL rather than
    // whatever the allocator left behind — freeing walks these fields, and it
    // has no other way to tell an unset one from a real reference.
    void *payload = header + 1;
    memset(payload, 0, size);

    return payload;
}

// Frees what an owning indirection names -- what 'new box T' allocates, and
// what every owning pointer field holds.
static void drop_box(Allocator allocator, const Type *type, void *value) {
    (void)type;

    void *owned;
    memcpy(&owned, value, sizeof(owned));

    object_free(allocator, owned);
}

// Frees what each element of a block owns, for a payload holding more than one
// value of its type. How many is the header's count: an array's length is not
// in its type, and the memory past it has never been written.
static void drop_elements(Allocator allocator, const Type *type, void *value, size_t count) {
    for (size_t i = 0; i < count; i++) {
        type->drop(allocator, type, (char *)value + i * type->size);
    }
}

// Frees what a value's fields own, so that freeing an object frees the tree
// beneath it. A field that owns nothing has no drop function and is skipped,
// so a struct of four ints walks four fields and calls nothing -- and a 'ref'
// field is skipped by the same test, having never owned what it names.
//
// An inline struct or string field is reached through its own function rather
// than being flattened into this walk, which is what keeps the offsets it
// needs its own business.
static void drop_fields(Allocator allocator, const Type *type, void *value) {
    for (size_t i = 0; i < type->field_count; i++) {
        const TypeField *field = &type->fields[i];

        if (field->type->drop) {
            field->type->drop(allocator, field->type, (char *)value + field->offset);
        }
    }
}

void object_select_drop(Type *type) {
    // Asked of the type rather than of its kind, so that a struct earns a drop
    // exactly when some field of it owns, and every 'ref' is excluded here
    // rather than being tested again on each free.
    if (!type_is_owned(type)) {
        type->drop = NULL;
        return;
    }

    type->drop = type->kind == TYPE_INDIRECT ? drop_box : drop_fields;
}

void object_free(Allocator allocator, void *payload) {
    if (!payload) {
        return;
    }

    ObjectHeader *header = object_of(payload);

    if (header->type->drop) {
        drop_elements(allocator, header->type, payload, header->count);
    }

    allocator.free(allocator.ctx, header);
}
