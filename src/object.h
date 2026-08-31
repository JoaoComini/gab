#ifndef GAB_OBJECT_H
#define GAB_OBJECT_H

#include "allocator.h"
#include "arena.h"
#include "type/type.h"
#include "type/type_layout.h"

typedef struct TypeRegistry TypeRegistry;

typedef struct {
    void *data;
    int32_t capacity;
    int32_t length;
} GabBlockValue;

typedef struct {
    GabBlockValue block;
} GabStringValue;

typedef struct {
    const char *data;
    int32_t length;
} GabStrRef;

typedef struct {
    void *data;
    int32_t length;
} GabArrayValue;

typedef struct ObjectHeader {
    const DropPlan *drop;
} ObjectHeader;

_Static_assert(sizeof(ObjectHeader) % 8 == 0, "the header must not misalign the payload that follows it");

_Static_assert(sizeof(size_t) >= 8, "an array's size computation assumes a 64-bit size_t");

bool block_reserve(const Allocator *allocator, GabBlockValue *block, int32_t extra, size_t stride);

const DropPlan *object_build_drop(Arena *arena, TypeRegistry *registry, const Type *type);

ObjectHeader *object_of(void *payload);

void *object_alloc(const Allocator *allocator, size_t size, const DropPlan *drop);

void object_release(const Allocator *allocator, const DropPlan *drop, void *value);

void object_free(const Allocator *allocator, void *payload);

#endif
