#include "object.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

ObjectHeader *object_of(void *payload) {
    assert(payload && "a header is only meaningful for a live object");

    return (ObjectHeader *)payload - 1;
}

void *object_alloc(Allocator allocator, const Type *type) {
    assert(type && "an object needs a type; freeing it walks its fields");

    size_t size = type->size;

    ObjectHeader *header = allocator.alloc(allocator.ctx, sizeof(ObjectHeader) + size);

    if (!header) {
        return NULL;
    }

    header->type = type;

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

// Frees the block an array header names, and everything its elements own. The
// count is the header's own 'length': the block records nothing, so this is the
// only thing that knows how far the live elements run.
//
// Its own function rather than a case in the field walk, because its bounds are
// not in the type -- which is the shape DropFn was given a type parameter for.
void object_drop_array(Allocator allocator, const Type *type, void *value) {
    GabArrayValue array;
    memcpy(&array, value, sizeof(array));

    if (!array.data) {
        return;
    }

    // What the 'data' pointer names, not anything the allocation says: a block
    // carries no header, so the type is the only thing that knows its stride.
    const Type *element = type_array_element(type);

    // Only when an element owns something itself. An array of ints frees its
    // block without touching a single element.
    if (element->drop) {
        for (int32_t i = 0; i < array.length; i++) {
            element->drop(allocator, element, (char *)array.data + (size_t)i * element->size);
        }
    }

    // Sized rather than recovered from a header: a block carries none, so how
    // many bytes it holds is the length beside the pointer times the width the
    // element type says.
    allocator.free_sized(allocator.ctx, array.data, (size_t)array.length * element->size);
}

// Frees the characters a string header owns. Its own function for the reason
// drop_array has one: what must be freed is the block the header names, which
// the field naming it cannot say now that the field is a raw address.
void object_drop_string(Allocator allocator, const Type *type, void *value) {
    (void)type;

    GabStringValue string;
    memcpy(&string, value, sizeof(string));

    if (!string.data) {
        return;
    }

    // Cast away the const the host reads its characters through: what is being
    // freed is the block, and only an owning header ever reaches here. Sized
    // for the reason drop_array gives -- a block has no header to ask.
    allocator.free_sized(allocator.ctx, (void *)(uintptr_t)string.data, (size_t)string.length);
}

void object_select_drop(Type *type) {
    assert(type->kind != TYPE_STRING && type->kind != TYPE_ARRAY &&
           "a header's drop is set where it is built, not inferred from its kind");

    // Asked of the type rather than of its kind, so that a struct earns a drop
    // exactly when some field of it owns, and every 'ref' is excluded here
    // rather than being tested again on each free.
    if (!type_is_owned(type)) {
        type->drop = NULL;
        return;
    }

    type->drop = type->kind == TYPE_BOX ? drop_box : drop_fields;
}

size_t type_release_width(const Type *type) {
    if (!type) {
        return 0;
    }

    // A header's length is as load-bearing as its pointer: a sized free reads
    // both, so both are cleared. Everything else that owns is reached through a
    // pointer, and clearing that is what makes it NULL.
    return type->kind == TYPE_ARRAY || type->kind == TYPE_STRING ? type->size : sizeof(void *);
}

void object_release(Allocator allocator, const Type *type, void *value) {
    // A type that owns nothing has no drop at all, which is what makes a
    // release of one free: the question was settled when its layout was.
    if (type && type->drop) {
        type->drop(allocator, type, value);
    }
}

void object_free(Allocator allocator, void *payload) {
    if (!payload) {
        return;
    }

    ObjectHeader *header = object_of(payload);

    if (header->type->drop) {
        header->type->drop(allocator, header->type, payload);
    }

    allocator.free(allocator.ctx, header);
}
