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

// Frees the memory a block names, at the capacity it carries. Nothing in it:
// which elements were ever written is not a question the capacity answers, so
// whatever counted them dropped them before this ran.
static void drop_block_at(Allocator allocator, const DropPlan *plan, void *value) {
    GabBlockValue block;
    memcpy(&block, value, sizeof(block));

    if (!block.data) {
        return;
    }

    allocator.free_sized(allocator.ctx, block.data, (size_t)block.capacity * plan->stride);
}

// Frees what the live elements of a block own, leaving the memory to the block
// itself. The two numbers come from different places: the count is a field of
// the value being dropped, and the address is inside the block field beside it.
//
// Runs before the block is freed, since it walks memory the block releases.
static void drop_prefix_at(Allocator allocator, const DropPlan *plan, void *value) {
    int32_t count;
    memcpy(&count, (char *)value + plan->count_offset, sizeof(count));

    GabBlockValue block;
    memcpy(&block, (char *)value + plan->block_offset, sizeof(block));

    if (!block.data) {
        return;
    }

    for (int32_t i = 0; i < count; i++) {
        drop_run(allocator, plan->inner, (char *)block.data + (size_t)i * plan->stride);
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
    case DROP_ARRAY:
        drop_array_at(allocator, plan, value);
        break;
    case DROP_BLOCK:
        drop_block_at(allocator, plan, value);
        break;
    case DROP_PREFIX:
        drop_prefix_at(allocator, plan, value);
        break;
    case DROP_FIELDS:
        drop_fields_at(allocator, plan, value);
        break;
    }
}

const DropPlan *drop_plan_live_prefix(Arena *arena, TypeRegistry *registry, const Type *type,
                                      size_t count_field, size_t block_field) {
    const TypeField *block = &type_fields(type)[block_field];
    const Type *element = type_pointee(block->type);

    const DropPlan *inner = type_registry_drop_of(registry, element);

    if (!inner) {
        return NULL;
    }

    const TypeLayout *layout = type_registry_layout_of(registry, type);

    DropPlan *plan = arena_alloc(arena, sizeof(DropPlan));

    *plan = (DropPlan){
        .kind = DROP_PREFIX,
        .inner = inner,
        .stride = type_registry_size_of(registry, element),
        .count_offset = layout->offsets[count_field],
        .block_offset = layout->offsets[block_field],
    };

    return plan;
}

const DropPlan *drop_plan_counted_block(Arena *arena, TypeRegistry *registry, const Type *type,
                                        size_t count_field, size_t block_field) {
    DropStep steps[GAB_MAX_DROP_STEPS];
    size_t count = 0;

    const DropPlan *prefix = drop_plan_live_prefix(arena, registry, type, count_field, block_field);

    // First, so that what the elements own is freed while the block naming them
    // is still allocated. The offsets it reads are its own, so the step sits at
    // the start of the value rather than at a field.
    if (prefix) {
        steps[count++] = (DropStep){.offset = 0, .plan = prefix};
    }

    for (size_t i = 0; i < type_field_count(type) && count < GAB_MAX_DROP_STEPS; i++) {
        DropStep step = drop_plan_field(registry, type, i);

        if (step.plan) {
            steps[count++] = step;
        }
    }

    return drop_plan_steps(arena, steps, count);
}

const DropPlan *drop_plan_steps(Arena *arena, const DropStep *steps, size_t count) {
    if (count == 0) {
        return NULL;
    }

    DropStep *copied = arena_alloc(arena, count * sizeof(DropStep));

    memcpy(copied, steps, count * sizeof(DropStep));

    DropPlan *plan = arena_alloc(arena, sizeof(DropPlan));

    *plan = (DropPlan){.kind = DROP_FIELDS, .steps = copied, .step_count = count};

    return plan;
}

DropStep drop_plan_field(TypeRegistry *registry, const Type *type, size_t field) {
    const TypeLayout *layout = type_registry_layout_of(registry, type);

    return (DropStep){
        .offset = layout->offsets[field],
        .plan = type_registry_drop_of(registry, type_fields(type)[field].type),
    };
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
