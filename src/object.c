#include "object.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

ObjectHeader *object_of(void *payload) {
    assert(payload && "a header is only meaningful for a live object");

    return (ObjectHeader *)payload - 1;
}

void *object_alloc(Allocator allocator, TypeHandle type) {
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
static void drop_box(Allocator allocator, TypeHandle type, void *value) {
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
static void drop_fields(Allocator allocator, TypeHandle type, void *value) {
    for (size_t i = 0; i < type_field_count(type); i++) {
        const TypeField *field = &type_fields(type)[i];

        if (field->type->drop) {
            field->type->drop(allocator, field->type, (char *)value + field->offset);
        }
    }
}

// Frees what an array's elements own. The elements live in the array itself, so
// there is no block to free -- only the run of them to walk, as many as the
// type says.
//
// Its own function rather than a case in the field walk, because an array has
// no fields: one element type repeated is what it holds, which is the shape
// DropFn was given a type parameter for.
void object_drop_array(Allocator allocator, TypeHandle type, void *value) {
    TypeHandle element = type_array_element(type);
    int32_t length = type_array_length(type);

    // Selected only when the element owns something, so reaching here at all
    // means there is something in each to free.
    for (int32_t i = 0; i < length; i++) {
        element->drop(allocator, element, (char *)value + (size_t)i * element->size);
    }
}

// Frees the characters a string header owns. Its own function for the reason
// drop_array has one: what must be freed is the block the header names, which
// the field naming it cannot say now that the field is a raw address.
void object_drop_string(Allocator allocator, TypeHandle type, void *value) {
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
    // Asked of the type rather than of its kind, so that a struct earns a drop
    // exactly when some field of it owns, and every 'ref' is excluded here
    // rather than being tested again on each free.
    //
    // Every constructed type comes through here, so which glue frees what is
    // decided in one place: a constructor builds a type and says nothing about
    // freeing it.
    if (!type_is_owned(type)) {
        type->drop = NULL;
        return;
    }

    switch (type->kind) {
    case TYPE_BOX:
        type->drop = drop_box;
        break;

    // A run of elements and a block of characters each know their own bounds,
    // which the field walk cannot read: one counts by a length its type says,
    // the other frees what an address names.
    case TYPE_ARRAY:
        type->drop = object_drop_array;
        break;
    case TYPE_STRING:
        type->drop = object_drop_string;
        break;

    default:
        type->drop = drop_fields;
        break;
    }
}

size_t type_release_width(TypeHandle type) {
    if (!type) {
        return 0;
    }

    // A header's length is as load-bearing as its pointer: a sized free reads
    // both, so both are cleared. Everything else that owns is reached through a
    // pointer, and clearing that is what makes it NULL.
    return type->kind == TYPE_ARRAY || type->kind == TYPE_STRING ? type->size : sizeof(void *);
}

void object_release(Allocator allocator, TypeHandle type, void *value) {
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
