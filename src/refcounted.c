#include "refcounted.h"

#include <assert.h>
#include <string.h>

RefCounted *gab_refcounted_of(void *payload) {
    assert(payload && "a header is only meaningful for a live object");

    return (RefCounted *)payload - 1;
}

void *gab_refcounted_alloc(Allocator allocator, const Type *type) {
    assert(type && "an object needs a type; release walks its fields");

    RefCounted *header = allocator.alloc(allocator.ctx, sizeof(RefCounted) + type->size);

    if (!header) {
        return NULL;
    }

    header->strong = 1;
    header->weak = 0;
    header->type = type;

    // Zeroed so that a pointer field nobody assigned is NULL rather than
    // whatever the allocator left behind — release walks these fields, and it
    // has no other way to tell an unset one from a real reference.
    void *payload = header + 1;
    memset(payload, 0, type->size);

    return payload;
}

void gab_retain(void *payload) {
    if (!payload) {
        return;
    }

    gab_refcounted_of(payload)->strong++;
}

// Releases the pointer fields a payload holds, so that freeing an object frees
// what it exclusively owned. Driven by the Type rather than by a tag on each
// value: the compiler already knows which fields are pointers, so a struct of
// four ints walks four fields and touches no refcount.
static void release_fields(Allocator allocator, const Type *type, char *payload) {
    for (size_t i = 0; i < type->field_count; i++) {
        const TypeField *field = &type->fields[i];

        if (type_is_pointer(field->type)) {
            void *reference;
            memcpy(&reference, payload + field->offset, sizeof(reference));

            gab_release(allocator, reference);
            continue;
        }

        // A struct field is stored inline, so its own pointer fields are part
        // of this payload and are released here rather than by a separate
        // object's teardown.
        if (field->type->kind == TYPE_STRUCT) {
            release_fields(allocator, field->type, payload + field->offset);
        }
    }
}

void gab_release(Allocator allocator, void *payload) {
    if (!payload) {
        return;
    }

    RefCounted *header = gab_refcounted_of(payload);

    assert(header->strong > 0 && "released an object that already reached zero");

    if (--header->strong > 0) {
        return;
    }

    release_fields(allocator, header->type, (char *)payload);

    // Freed outright because nothing holds a weak reference yet. Once 'weak *T'
    // exists this is where the header outlives the payload instead: zero the
    // payload, and free the header only when the weak count also reaches zero.
    assert(header->weak == 0 && "weak references exist but nothing implements them yet");

    allocator.free(allocator.ctx, header);
}
