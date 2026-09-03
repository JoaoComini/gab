#ifndef GAB_LINK_H
#define GAB_LINK_H

#include "arena.h"
#include "diagnostics.h"
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

typedef struct GabCtx GabCtx;
typedef void (*GabExternFn)(GabCtx *args);

typedef struct GabCtx Args;

typedef struct {
    GabExternFn body;

    const struct Function *function;

    /* Each parameter's byte offset from the frame base, so reading one does not walk those before it. */
    const size_t *param_offsets;

    /* The size of each type argument the specialization chose, and the element size of each array
     * parameter, both fixed once the binding resolves. */
    const size_t *type_arg_sizes;
    const size_t *param_strides;
} ExternProto;

GAB_LIST(ExternProtoList, extern_proto_list, ExternProto)

typedef struct {
    Chunk *chunk;

    int arity;
    int max_registers;

    FrameRefList refs;
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
    size_t local_index;
    const struct Function *function;
    Span span;
} ExternRequest;

GAB_LIST(ExternRequestList, extern_request_list, ExternRequest)

typedef struct {
    FuncPrototype top_level;

    FuncProtoList prototypes;
    ExternProtoList extern_protos;

    TypeList types;
    HeapShapeList type_shapes;
    StringList strings;

    RelocationList proto_relocations;
    RelocationList extern_relocations;
    RelocationList type_relocations;
    RelocationList string_relocations;

    ProtoBindingList bindings;
    ExternRequestList externs;

    Arena *arena;

    size_t *type_map;
    size_t *string_map;
} Unit;

void unit_free(Unit *unit);

GAB_LIST(TopLevelList, top_level_list, FuncPrototype)

typedef struct {
    String *module;
    String *owner;
    String *name;
    GabExternFn fn;
} ExternBinding;

GAB_LIST(ExternBindingList, extern_binding_list, ExternBinding)

typedef struct {
    FuncProtoList prototypes;

    ExternProtoList extern_protos;

    HeapShapeList heap_shapes;

    TypeList shape_types;

    StringList strings;

    TopLevelList top_levels;

    ExternBindingList extern_bindings;
} Program;

bool link_check(Program *program, Unit *unit, TypeRegistry *registry, Diagnostics *diagnostics);

void link_install(Program *program, Unit *unit);

#endif
