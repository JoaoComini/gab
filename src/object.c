#include "object.h"

#include <assert.h>
#include <string.h>

ObjectHeader *gab_object_of(void *payload) {
    assert(payload && "a header is only meaningful for a live object");

    return (ObjectHeader *)payload - 1;
}

void *gab_object_alloc(Allocator allocator, const Type *type) {
    assert(type && "an object needs a type; freeing it walks its fields");

    ObjectHeader *header = allocator.alloc(allocator.ctx, sizeof(ObjectHeader) + type->size);

    if (!header) {
        return NULL;
    }

    header->type = type;

    // Zeroed so that a pointer field nobody assigned is NULL rather than
    // whatever the allocator left behind — freeing walks these fields, and it
    // has no other way to tell an unset one from a real reference.
    void *payload = header + 1;
    memset(payload, 0, type->size);

    return payload;
}

// Frees the objects a payload's fields own, so that freeing an object frees the
// tree beneath it. Driven by the Type rather than by a tag on each value: the
// compiler already knows which fields are pointers, so a struct of four ints
// walks four fields and frees nothing.
static void free_fields(Allocator allocator, const Type *type, char *payload) {
    // A payload that is itself an owning pointer -- what 'new box T' allocates.
    // It has no fields to walk, so the whole of what it owns is the one pointer
    // sitting in it, and skipping this would leak everything beneath it.
    if (type_is_pointer(type)) {
        if (type->is_ref) {
            return;
        }

        void *owned;
        memcpy(&owned, payload, sizeof(owned));

        gab_object_free(allocator, owned);
        return;
    }

    for (size_t i = 0; i < type->field_count; i++) {
        const TypeField *field = &type->fields[i];

        if (type_is_pointer(field->type)) {
            // A 'ref T' field borrows: something else owns what it names, and
            // following it here would free an object still in use.
            if (field->type->is_ref) {
                continue;
            }

            void *owned;
            memcpy(&owned, payload + field->offset, sizeof(owned));

            gab_object_free(allocator, owned);
            continue;
        }

        // A struct field is stored inline, so its own pointer fields are part
        // of this payload and are freed here rather than by a separate object's
        // teardown.
        if (field->type->kind == TYPE_STRUCT) {
            free_fields(allocator, field->type, payload + field->offset);
        }
    }
}

void gab_object_free(Allocator allocator, void *payload) {
    if (!payload) {
        return;
    }

    ObjectHeader *header = gab_object_of(payload);

    free_fields(allocator, header->type, (char *)payload);

    allocator.free(allocator.ctx, header);
}
