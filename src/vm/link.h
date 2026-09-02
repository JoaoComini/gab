#ifndef GAB_LINK_H
#define GAB_LINK_H

#include "arena.h"
#include "diagnostics.h"
#include "string/string.h"
#include "type/type.h"
#include "type/type_layout.h"
#include "util/list.h"
#include "vm/chunk.h"

#include <stddef.h>

struct Binding;
struct Type;
struct TypeRegistry;

typedef struct {
    unsigned int slot;

    const DropPlan *drop;
    size_t release_width;
} FrameRef;

GAB_LIST(FrameRefList, frame_ref_list, FrameRef)

typedef struct GabArgs GabArgs;
typedef void (*GabExternFn)(GabArgs *args);

typedef struct GabArgs Args;

typedef struct FfiSignature FfiSignature;

typedef struct {
    GabExternFn body;

    /* Set when the extern names a raw C symbol; 'body' is then NULL and the call goes through libffi. */
    const FfiSignature *signature;

    const struct Function *function;
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

    /* The raw C symbol, when this binding was made by gab_extern_c rather than gab_extern. */
    void *symbol;

    /* Whether the symbol takes a GabCtx * ahead of the parameters its declaration names. */
    bool wants_ctx;
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

bool link_check(Program *program, Unit *unit, Arena *arena, struct TypeRegistry *types,
                Diagnostics *diagnostics);

void link_install(Program *program, Unit *unit);

#endif
