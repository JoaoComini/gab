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

    // The payload dies now; the header may not. A weak reference points at the
    // payload address and asks 'is this still alive?' by reading the strong
    // count behind it, so the header has to outlive whatever it describes —
    // Swift's side-table arrangement, and why 'weak' is a count rather than a
    // flag.
    //
    // Zeroed rather than left as-is so that reading a dead payload finds
    // nothing meaningful, and so a debugger shows the death rather than stale
    // contents. strong is already 0, which is what records that it happened.
    memset(payload, 0, header->type->size);

    if (header->weak > 0) {
        return;
    }

    allocator.free(allocator.ctx, header);
}

void gab_retain_weak(void *payload) {
    if (!payload) {
        return;
    }

    gab_refcounted_of(payload)->weak++;
}

void gab_release_weak(Allocator allocator, void *payload) {
    if (!payload) {
        return;
    }

    RefCounted *header = gab_refcounted_of(payload);

    assert(header->weak > 0 && "released a weak reference that was never taken");

    if (--header->weak > 0) {
        return;
    }

    // The last weak reference to an object whose payload is already gone: this
    // is what finally frees the header the payload's death left behind. An
    // object still strongly held keeps its header either way.
    if (header->strong == 0) {
        allocator.free(allocator.ctx, header);
    }
}

bool gab_is_alive(const void *payload) {
    if (!payload) {
        return false;
    }

    return gab_refcounted_of((void *)payload)->strong > 0;
}
