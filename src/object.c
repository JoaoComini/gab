#include "object.h"

#include "arena.h"
#include "type_registry.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

ObjectHeader *object_of(void *payload) {
    assert(payload && "a header is only meaningful for a live object");

    return (ObjectHeader *)payload - 1;
}

void *object_alloc(Allocator allocator, size_t size, const DropPlan *drop) {
    ObjectHeader *header = allocator.alloc(allocator.ctx, sizeof(ObjectHeader) + size);

    if (!header) {
        return NULL;
    }

    header->drop = drop;

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

// Frees what the live elements of a block own, then the memory itself.
//
// Both numbers are the block's own: the length says how far anything was
// written, the capacity what the memory was taken at. Walking before freeing is
// the whole of the ordering, and it is local here rather than spread across two
// steps that had to be sequenced.
static void drop_block_at(Allocator allocator, const DropPlan *plan, void *value) {
    GabBlockValue block;
    memcpy(&block, value, sizeof(block));

    if (!block.data) {
        return;
    }

    // Only when the element owns something. A block of ints has no inner plan,
    // and freeing the memory is the whole of what it does.
    if (plan->inner) {
        for (int32_t i = 0; i < block.length; i++) {
            drop_run(allocator, plan->inner, (char *)block.data + (size_t)i * plan->stride);
        }
    }

    allocator.free_sized(allocator.ctx, block.data, (size_t)block.capacity * plan->stride);
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
    case DROP_ARRAY:
        drop_array_at(allocator, plan, value);
        break;
    case DROP_BLOCK:
        drop_block_at(allocator, plan, value);
        break;
    case DROP_FIELDS:
        drop_fields_at(allocator, plan, value);
        break;
    }
}

// What an empty block's first allocation holds. Small enough that a collection
// appended to once does not reserve a page, large enough that the first few
// appends do not each reallocate.
#define BLOCK_INITIAL_CAPACITY 8

bool block_reserve(Allocator allocator, GabBlockValue *block, int32_t extra, size_t stride) {
    if (extra <= 0) {
        return true;
    }

    // Checked before the sum, which would otherwise be the overflow it is
    // guarding against.
    if (block->length > INT32_MAX - extra) {
        return false;
    }

    int32_t needed = block->length + extra;

    if (needed <= block->capacity) {
        return true;
    }

    // Doubling overflows a signed int past 2^30 elements, which is undefined
    // rather than merely wrong.
    if (block->capacity > INT32_MAX / 2) {
        return false;
    }

    int32_t capacity = block->capacity ? block->capacity * 2 : BLOCK_INITIAL_CAPACITY;

    // Doubling may still fall short, since 'extra' is not bounded by the
    // capacity: a long append onto a short block clears it in one step rather
    // than looping.
    if (capacity < needed) {
        capacity = needed;
    }

    void *memory = allocator.alloc(allocator.ctx, (size_t)capacity * stride);

    if (!memory) {
        return false;
    }

    if (block->length) {
        memcpy(memory, block->data, (size_t)block->length * stride);
    }

    // Zeroed past the live ones for the reason every allocation is: an element
    // that owns and was never written must read as NULL, since the drop walk
    // has no other way to tell it from a live reference.
    memset((char *)memory + (size_t)block->length * stride, 0,
           ((size_t)capacity - (size_t)block->length) * stride);

    if (block->data) {
        allocator.free_sized(allocator.ctx, block->data, (size_t)block->capacity * stride);
    }

    block->data = memory;
    block->capacity = capacity;

    return true;
}

// Derives the plan for a type whose layout is known, or leaves it NULL when the
// type owns nothing.
//
// Derivation answers for every shape that implies how it frees: a struct owns
// through its fields, an array through its elements, a box through its pointee.
// A type holding a block is the shape that does not imply it -- see
// drop_plan_live_prefix -- and supplies its own plan instead.
//
// Every constructed type comes through here, headers included: what a free has
// to do follows from the kind, and whether anything is freed at all follows
// from type_is_owned. A constructor builds a type and says nothing about
// freeing it.
const DropPlan *object_build_drop(Arena *arena, TypeRegistry *registry, const Type *type) {
    if (!type_is_owned(type)) {
        return NULL;
    }

    DropPlan *plan = arena_alloc(arena, sizeof(DropPlan));

    *plan = (DropPlan){.kind = DROP_FIELDS};

    switch (type->kind) {
    case TYPE_BOX:
        plan->kind = DROP_BOX;

        // Nothing more. What the block owns is read off its own header when it
        // is freed, so the slot naming it carries no inner plan -- which is
        // also what lets a ring through a 'box' be planned at all, neither end
        // waiting on the other.
        plan->inner = NULL;
        break;

    // A run of elements and a block of characters each know bounds the field
    // walk cannot read: one counts by a length its type says, the other frees
    // what an address names.
    case TYPE_ARRAY:
        plan->kind = DROP_ARRAY;
        plan->inner = type_registry_drop_of(registry, type_array_element(type));
        plan->stride = type_registry_size_of(registry, type_array_element(type));
        plan->length = type_array_length(type);
        break;

    // The memory and nothing in it: a block is a capacity, and what has been
    // written into it is counted by whatever holds the block.
    case TYPE_BLOCK:
        plan->kind = DROP_BLOCK;
        plan->inner = type_registry_drop_of(registry, type_pointee(type));
        plan->stride = type_registry_size_of(registry, type_pointee(type));
        break;

    default: {
        // Only the fields that own become steps: what a free walks is the shape
        // of what owns rather than the shape of the type.
        size_t owning = 0;

        for (size_t i = 0; i < type_field_count(type); i++) {
            if (type_registry_drop_of(registry, type_fields(type)[i].type)) {
                owning++;
            }
        }

        DropStep *steps = arena_alloc(arena, owning * sizeof(DropStep));
        size_t count = 0;

        const TypeLayout *layout = type_registry_layout_of(registry, type);

        for (size_t i = 0; i < type_field_count(type); i++) {
            const DropPlan *inner = type_registry_drop_of(registry, type_fields(type)[i].type);

            if (!inner) {
                continue;
            }

            steps[count++] = (DropStep){.offset = layout->offsets[i], .plan = inner};
        }

        plan->steps = steps;
        plan->step_count = count;
        break;
    }
    }

    return plan;
}

void object_release(Allocator allocator, const DropPlan *drop, void *value) {
    // A type that owns nothing has no plan at all, which is what makes a
    // release of one free: the question was settled where its layout was.
    drop_run(allocator, drop, value);
}

void object_free(Allocator allocator, void *payload) {
    if (!payload) {
        return;
    }

    ObjectHeader *header = object_of(payload);

    drop_run(allocator, header->drop, payload);

    allocator.free(allocator.ctx, header);
}
