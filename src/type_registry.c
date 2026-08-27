#include "type_registry.h"

#include "arena.h"
#include "object.h"
#include "string/string.h"
#include "type.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static Type *register_builtin(TypeRegistry *registry, TypeKind kind, const char *name, size_t size,
                              size_t alignment) {
    Type *type = type_create(registry->arena, kind, string_from_cstr(registry->strings, name));
    type->size = size;
    type->alignment = alignment;

    return type;
}

// The 'String' type, laid out from its two fields. Whether the header owns the
// characters it names is the whole difference between a string and a view of
// one, and it is the header's own business: the raw address naming them cannot
// say.
static Type *string_builtin_create(TypeRegistry *registry, bool is_ref) {
    // Each half carries its own name: 'String' owns its characters, 'str'
    // borrows someone else's. Two names rather than one, because the two differ
    // in what a slot holding them must free, and a diagnostic naming both
    // 'String' would read as a mismatch between a type and itself.
    const char *name = is_ref ? "str" : "String";
    Type *type = type_struct_create(registry->arena, string_from_cstr(registry->strings, name), 2);
    type->kind = TYPE_STRING;

    // A raw address either way: who frees the characters is the header's own
    // business rather than this field's.
    Type *characters = type_registry_ptr_to(registry, registry->builtins.byte_type);

    type_add_field(type, string_from_cstr(registry->strings, "data"), characters);
    type_add_field(type, string_from_cstr(registry->strings, "length"), registry->builtins.int_type);
    type_layout_compute(type);

    // The whole difference between the two, and all of it: one frees the
    // characters it names and one was never given anything to free.
    //
    // Set here rather than by object_select_drop, which every other constructed
    // type goes through: that reads type_is_owned to choose, and a string's
    // ownership is this assignment. Nothing else distinguishes the two -- they
    // share a kind and a layout -- so the question would have no other answer.
    // Spelling the borrowed one as its own constructor is what would let it
    // join the rest.
    type->drop = is_ref ? NULL : object_drop_string;

    return type;
}

void type_registry_register_builtins(TypeRegistry *registry) {
    registry->builtins.int_type = register_builtin(registry, TYPE_INT, "int", 4, 4);
    registry->builtins.float_type = register_builtin(registry, TYPE_FLOAT, "float", 4, 4);
    registry->builtins.bool_type = register_builtin(registry, TYPE_BOOL, "bool", 1, 1);

    // Sized as one byte, which is the stride a walk over characters advances by
    // and the unit a block of them is counted in.
    registry->builtins.byte_type = register_builtin(registry, TYPE_INT, "byte", 1, 1);

    // A struct in its layout and a builtin in its semantics: the fields are
    // where its size, its alignment and what it owns all come from, while
    // comparison, literals and the borrow spelling stay the kind's own.
    registry->builtins.string_type = string_builtin_create(registry, false);

    // Same layout, differing only in the field naming the characters, which
    // borrows here and owns there. 'ref String' is a different type again: the
    // address of a slot holding a header, which is what 'ref' means everywhere.
    registry->builtins.str_type = string_builtin_create(registry, true);

    registry->builtins.str_type->owner = registry->builtins.string_type;

    // The VM and the host both read these two fields as GabStringValue, so the
    // computed layout and the C struct are two statements of one thing.
    assert(registry->builtins.string_type->size == sizeof(GabStringValue));
    assert(registry->builtins.string_type->alignment == _Alignof(GabStringValue));

    // The bare name every 'Array T' is interned under. Sized as the header the
    // elements make it, so that a diagnostic naming it says something true even
    // though no slot ever holds this type itself.
    registry->builtins.array_type = register_builtin(registry, TYPE_ARRAY, "Array", 0, 1);

    // Poison type. Deliberately never given a name in any scope: no script can
    // name it, it only arises from a failed resolution.
    registry->builtins.error_type = register_builtin(registry, TYPE_ERROR, "<error>", 0, 1);
}

TypeRegistry *type_registry_create(Arena *arena, StringPool *strings) {
    TypeRegistry *registry = arena_alloc(arena, sizeof(TypeRegistry));
    registry->applications =
        type_app_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->arena = arena;
    registry->strings = strings;
    type_registry_register_builtins(registry);

    return registry;
}

void type_registry_destroy(TypeRegistry *registry) { type_app_map_destroy(registry->applications); }

// Looks an application up, and interns the argument list if it is new. The
// caller builds its key on the stack, so the arguments are copied into the
// arena before an entry can hold them.
static Type **application_lookup(TypeRegistry *registry, TypeApp app) {
    return type_app_map_lookup(registry->applications, app);
}

static void application_insert(TypeRegistry *registry, TypeApp app, Type *type) {
    TypeArg *args = arena_alloc(registry->arena, app.arg_count * sizeof(TypeArg));
    memcpy(args, app.args, app.arg_count * sizeof(TypeArg));

    app.args = args;
    type->app = app;

    type_app_map_insert(registry->applications, app, type);
}

Type *type_registry_array_of(TypeRegistry *registry, Type *element, int32_t length) {
    TypeArg args[] = {
        {.kind = TYPE_ARG_TYPE, .type = element},
        {.kind = TYPE_ARG_CONST, .value = length},
    };

    TypeApp app = {
        .ctor = TYPE_CTOR_NOMINAL,
        .decl = registry->builtins.array_type,
        .args = args,
        .arg_count = 2,
    };

    Type **existing = application_lookup(registry, app);
    if (existing) {
        return *existing;
    }

    // The elements live in the array itself, so its width is the run of them
    // and its alignment is one element's. No header, no block: an 'Array T,N'
    // is laid out exactly as a C 'T[N]' is, which is what lets a host lay one
    // out with sizeof.
    Type *type = type_create(registry->arena, TYPE_ARRAY, registry->builtins.array_type->name);

    type->size = element->size * (size_t)length;
    type->alignment = element->alignment;

    // Every array answers the same methods, which do not depend on the element.
    // Reached through 'owner' rather than copied into each set, the way a 'str'
    // reaches a 'String's.
    type->owner = registry->builtins.array_type;

    // Interned before the drop is selected, so a recursive element -- an array
    // of a struct holding one -- finds this entry rather than building a
    // second.
    application_insert(registry, app, type);

    object_select_drop(type);

    return type;
}

// The three built-in one-argument constructors, which differ only in the kind
// the result carries. Written once because everything else about building one
// -- the width, the alignment, the pointee -- is the same for all of them.
static Type *indirect_to(TypeRegistry *registry, TypeCtor ctor, TypeKind kind, Type *inner) {
    TypeArg args[] = {{.kind = TYPE_ARG_TYPE, .type = inner}};

    TypeApp app = {.ctor = ctor, .decl = NULL, .args = args, .arg_count = 1};

    Type **existing = application_lookup(registry, app);
    if (existing) {
        return *existing;
    }

    // No name: an indirection is structural, so its printable form is derived
    // from the inner when a diagnostic asks. See Type::name.
    Type *type = type_create(registry->arena, kind, NULL);

    // Always a raw address to the payload, so a stack pointer and a heap one
    // are byte-identical; only the type says which is which. A 'ref T' is the
    // same address too -- what differs is who frees the inner.
    //
    // Computed here rather than stored, so a pointee that one day needs a
    // length beside its address changes this line and nothing else.
    type->size = sizeof(void *);
    type->alignment = _Alignof(void *);
    type->inner = inner;

    // Interned before the drop is selected, so a pointee reaching back here --
    // a struct holding a pointer to its own type -- finds this entry rather
    // than building a second.
    application_insert(registry, app, type);

    object_select_drop(type);

    return type;
}

Type *type_registry_box_to(TypeRegistry *registry, Type *inner) {
    return indirect_to(registry, TYPE_CTOR_BOX, TYPE_BOX, inner);
}

Type *type_registry_ref_to(TypeRegistry *registry, Type *inner) {
    return indirect_to(registry, TYPE_CTOR_REF, TYPE_REF, inner);
}

Type *type_registry_ptr_to(TypeRegistry *registry, Type *pointee) {
    return indirect_to(registry, TYPE_CTOR_PTR, TYPE_PTR, pointee);
}

Type *type_registry_error_type(TypeRegistry *registry) { return registry->builtins.error_type; }

Type *type_registry_get_builtin(TypeRegistry *registry, TypeKind kind) {
    switch (kind) {
    case TYPE_INT:
        return registry->builtins.int_type;
    case TYPE_FLOAT:
        return registry->builtins.float_type;
    case TYPE_BOOL:
        return registry->builtins.bool_type;
    case TYPE_STRING:
        return registry->builtins.string_type;
    default:
        break;
    }

    assert(0 && "type is not a builtin type");
    abort();
}
