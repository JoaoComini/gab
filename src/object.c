#include "object.h"

#include "arena.h"
#include "type/type_registry.h"

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

    void *payload = header + 1;
    memset(payload, 0, size);

    return payload;
}

static void drop_run(Allocator allocator, const DropPlan *plan, void *value);

static void drop_box_at(Allocator allocator, const DropPlan *plan, void *value) {
    (void)plan;

    void *owned;
    memcpy(&owned, value, sizeof(owned));

    object_free(allocator, owned);
}

static void drop_array_at(Allocator allocator, const DropPlan *plan, void *value) {
    for (int32_t i = 0; i < plan->length; i++) {
        drop_run(allocator, plan->inner, (char *)value + (size_t)i * plan->stride);
    }
}

static void drop_block_at(Allocator allocator, const DropPlan *plan, void *value) {
    GabBlockValue block;
    memcpy(&block, value, sizeof(block));

    if (!block.data) {
        return;
    }

    if (plan->inner) {
        for (int32_t i = 0; i < block.length; i++) {
            drop_run(allocator, plan->inner, (char *)block.data + (size_t)i * plan->stride);
        }
    }

    allocator.free_sized(allocator.ctx, block.data, (size_t)block.capacity * plan->stride);
}

static void drop_fields_at(Allocator allocator, const DropPlan *plan, void *value) {
    for (size_t i = 0; i < plan->step_count; i++) {
        const DropStep *step = &plan->steps[i];

        drop_run(allocator, step->plan, (char *)value + step->offset);
    }
}

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

#define BLOCK_INITIAL_CAPACITY 8

bool block_reserve(Allocator allocator, GabBlockValue *block, int32_t extra, size_t stride) {
    if (extra <= 0) {
        return true;
    }

    if (block->length > INT32_MAX - extra) {
        return false;
    }

    int32_t needed = block->length + extra;

    if (needed <= block->capacity) {
        return true;
    }

    if (block->capacity > INT32_MAX / 2) {
        return false;
    }

    int32_t capacity = block->capacity ? block->capacity * 2 : BLOCK_INITIAL_CAPACITY;

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

    memset((char *)memory + (size_t)block->length * stride, 0,
           ((size_t)capacity - (size_t)block->length) * stride);

    if (block->data) {
        allocator.free_sized(allocator.ctx, block->data, (size_t)block->capacity * stride);
    }

    block->data = memory;
    block->capacity = capacity;

    return true;
}

const DropPlan *object_build_drop(Arena *arena, TypeRegistry *registry, const Type *type) {
    if (!type_registry_owns(registry, type)) {
        return NULL;
    }

    DropPlan *plan = arena_alloc(arena, sizeof(DropPlan));

    *plan = (DropPlan){.kind = DROP_FIELDS};

    switch (type_kind(type)) {
    case TYPE_BOX:
        plan->kind = DROP_BOX;

        plan->inner = NULL;
        break;

    case TYPE_ARRAY:
        plan->kind = DROP_ARRAY;
        plan->inner = type_registry_drop_of(registry, type_array_element(type));
        plan->stride = type_registry_size_of(registry, type_array_element(type));
        plan->length = type_array_length(type);
        break;

    case TYPE_BLOCK:
        plan->kind = DROP_BLOCK;
        plan->inner = type_registry_drop_of(registry, type_pointee(type));
        plan->stride = type_registry_size_of(registry, type_pointee(type));
        break;

    default: {
        size_t owning = 0;

        const TypeFields *fields = type_registry_fields_of(registry, type);

        for (size_t i = 0; i < fields->count; i++) {
            if (type_registry_drop_of(registry, fields->fields[i].type)) {
                owning++;
            }
        }

        DropStep *steps = arena_alloc(arena, owning * sizeof(DropStep));
        size_t count = 0;

        const TypeLayout *layout = type_registry_layout_of(registry, type);

        for (size_t i = 0; i < fields->count; i++) {
            const DropPlan *inner = type_registry_drop_of(registry, fields->fields[i].type);

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
