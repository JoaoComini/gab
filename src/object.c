#include "object.h"

#include "arena.h"

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

static void drop_run(Allocator allocator, const DropPlan *plan, void *value);

// Frees what an owning indirection names -- what 'new box T' allocates, and
// what every owning pointer field holds.
static void drop_box_at(Allocator allocator, const DropPlan *plan, void *value) {
    (void)plan;

    void *owned;
    memcpy(&owned, value, sizeof(owned));

    object_free(allocator, owned);
}

// Frees the characters a string header owns. What must be freed is the block
// the header names, which the address alone cannot describe: the count sits in
// the slot beside it, since a block has no header to ask.
static void drop_string_at(Allocator allocator, void *value) {
    GabStringValue string;
    memcpy(&string, value, sizeof(string));

    if (!string.data) {
        return;
    }

    // Cast away the const the host reads its characters through: what is being
    // freed is the block, and only an owning header ever reaches here.
    allocator.free_sized(allocator.ctx, (void *)(uintptr_t)string.data, (size_t)string.length);
}

// Frees what an array's elements own. The elements live in the array itself, so
// there is no block to free -- only the run of them to walk, striding by a
// width the plan carries.
static void drop_array_at(Allocator allocator, const DropPlan *plan, void *value) {
    // Planned only when the element owns something, so reaching here at all
    // means there is something in each to free.
    for (int32_t i = 0; i < plan->length; i++) {
        drop_run(allocator, plan->inner, (char *)value + (size_t)i * plan->stride);
    }
}

// Frees what a value's fields own, so that freeing an object frees the tree
// beneath it. Only the fields that own are steps at all, so a struct of four
// ints has no plan and a struct of one owning field among forty has one step.
static void drop_fields_at(Allocator allocator, const DropPlan *plan, void *value) {
    for (size_t i = 0; i < plan->step_count; i++) {
        const DropStep *step = &plan->steps[i];

        drop_run(allocator, step->plan, (char *)value + step->offset);
    }
}

// Runs one plan against one value. The only walk on the free path, and it reads
// nothing but the plan: every offset and every width it needs was baked in
// where the layout was computed.
static void drop_run(Allocator allocator, const DropPlan *plan, void *value) {
    if (!plan) {
        return;
    }

    switch (plan->kind) {
    case DROP_BOX:
        drop_box_at(allocator, plan, value);
        break;
    case DROP_STRING:
        drop_string_at(allocator, value);
        break;
    case DROP_ARRAY:
        drop_array_at(allocator, plan, value);
        break;
    case DROP_FIELDS:
        drop_fields_at(allocator, plan, value);
        break;
    }
}

// Builds the plan for a type whose layout is known, or leaves it NULL when the
// type owns nothing.
//
// Every constructed type comes through here, headers included: what a free has
// to do follows from the kind, and whether anything is freed at all follows
// from type_is_owned. A constructor builds a type and says nothing about
// freeing it.
void object_select_drop(Arena *arena, Type *type) {
    if (!type_is_owned(type)) {
        type->drop = NULL;
        return;
    }

    DropPlan *plan = arena_alloc(arena, sizeof(DropPlan));

    *plan = (DropPlan){.kind = DROP_FIELDS};

    switch (type->kind) {
    case TYPE_BOX:
        plan->kind = DROP_BOX;

        // The pointee's own plan, so freeing the block also frees what the
        // value in it owns. NULL where it owns nothing, which is what makes
        // freeing a 'box int' the block and nothing more.
        plan->inner = type->indirect.pointee ? type->indirect.pointee->drop : NULL;
        break;

    // A run of elements and a block of characters each know bounds the field
    // walk cannot read: one counts by a length its type says, the other frees
    // what an address names.
    case TYPE_ARRAY:
        plan->kind = DROP_ARRAY;
        plan->inner = type_array_element(type)->drop;
        plan->stride = type_array_element(type)->size;
        plan->length = type_array_length(type);
        break;

    case TYPE_STRING:
        plan->kind = DROP_STRING;
        break;

    default: {
        // Only the fields that own become steps: what a free walks is the shape
        // of what owns rather than the shape of the type.
        size_t owning = 0;

        for (size_t i = 0; i < type->record.field_count; i++) {
            if (type->record.fields[i].type->drop) {
                owning++;
            }
        }

        DropStep *steps = arena_alloc(arena, owning * sizeof(DropStep));
        size_t count = 0;

        for (size_t i = 0; i < type->record.field_count; i++) {
            const TypeField *field = &type->record.fields[i];

            if (!field->type->drop) {
                continue;
            }

            steps[count++] = (DropStep){.offset = field->offset, .plan = field->type->drop};
        }

        plan->steps = steps;
        plan->step_count = count;
        break;
    }
    }

    type->drop = plan;
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
    // A type that owns nothing has no plan at all, which is what makes a
    // release of one free: the question was settled where its layout was.
    if (type) {
        drop_run(allocator, type->drop, value);
    }
}

void object_free(Allocator allocator, void *payload) {
    if (!payload) {
        return;
    }

    ObjectHeader *header = object_of(payload);

    drop_run(allocator, header->type->drop, payload);

    allocator.free(allocator.ctx, header);
}
