#ifndef GAB_LINK_H
#define GAB_LINK_H

#include "diagnostics.h"
#include "memory/arena.h"
#include "string/string.h"
#include "type/type.h"
#include "type/type_layout.h"
#include "type/type_registry.h"
#include "util/list.h"
#include "vm/chunk.h"

#include <stddef.h>

struct Binding;
struct Type;

typedef struct {
    unsigned int slot;

    const DropPlan *drop;
    size_t release_width;
} FrameRef;

GAB_LIST(FrameRefList, frame_ref_list, FrameRef)

typedef struct {
    Chunk *chunk;

    int arity;
    int max_registers;

    FrameRefList refs;

    /* For a script's body, the slot its first declaration was given; a caller reading what the script
     * produced looks there rather than assuming where a slot lands. */
    int result_slot;
} FuncPrototype;

void func_proto_free(FuncPrototype *proto);

GAB_LIST(FuncProtoList, func_proto_list, FuncPrototype *)

typedef struct HeapShape {
    size_t size;

    const DropPlan *drop;

    size_t release_width;
} HeapShape;

GAB_LIST(HeapShapeList, heap_shape_list, HeapShape)

GAB_LIST(StringList, string_list, String *)

typedef struct {
    Chunk *chunk;
    size_t offset;
} Relocation;

GAB_LIST(RelocationList, relocation_list, Relocation)

typedef struct {
    struct Function *function;
    size_t local_index;
} ProtoBinding;

GAB_LIST(ProtoBindingList, proto_binding_list, ProtoBinding)

typedef struct {
    FuncPrototype top_level;

    FuncProtoList prototypes;

    TypeList types;
    HeapShapeList type_shapes;
    StringList strings;

    RelocationList proto_relocations;
    RelocationList type_relocations;
    RelocationList string_relocations;

    ProtoBindingList bindings;

    Arena *arena;

    size_t *type_map;
    size_t *string_map;
} ObjectFile;

/* Moves the top level out, leaving the unit safe to free. */
void object_file_take_top_level(ObjectFile *unit, FuncPrototype *out);

void object_file_free(ObjectFile *unit);

GAB_LIST(TopLevelList, top_level_list, FuncPrototype)

typedef struct {
    FuncProtoList prototypes;

    HeapShapeList heap_shapes;

    TypeList shape_types;

    StringList strings;

    TopLevelList top_levels;

} Program;

bool link_check(Program *program, ObjectFile *unit, Diagnostics *diagnostics);

void link_install(Program *program, ObjectFile *unit);

#endif
