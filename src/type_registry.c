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
    String *interned = string_from_cstr(registry->strings, name);

    Type *type = type_create(registry->arena, kind, interned);
    type->size = size;
    type->alignment = alignment;

    // Interned like every other type, under no scope: a builtin belongs to no
    // module, and its name is the whole of its identity.
    nominal_key_insert(registry->nominals, (NominalKey){.scope = NULL, .name = interned}, type);

    return type;
}

// The 'String' type, laid out from its two fields. Whether the header owns the
// characters it names is the whole difference between a string and a view of
// one, and it is the header's own business: the raw address naming them cannot
// say.
static Type *string_builtin_create(TypeRegistry *registry) {
    Type *type = type_struct_create(registry->arena, string_from_cstr(registry->strings, "String"), 2);
    type->kind = TYPE_STRING;

    // A raw address either way: who frees the characters is the header's own
    // business rather than this field's.
    TypeHandle characters = type_registry_ptr_to(registry, registry->builtins.byte_type);

    type_add_field(type, string_from_cstr(registry->strings, "data"), characters);
    type_add_field(type, string_from_cstr(registry->strings, "length"), registry->builtins.int_type);
    type_layout_compute(type);

    // A string owns its characters, always: what borrows them is a 'ref str',
    // which owns nothing because no reference does. So this is not a question
    // the type has to be asked -- it is what being a String means, and
    // object_select_drop reads it off the kind.
    object_select_drop(registry->arena, type);

    return type;
}

void type_registry_register_builtins(TypeRegistry *registry) {
    registry->builtins.int_type = register_builtin(registry, TYPE_INT, "int", 4, 4);
    registry->builtins.float_type = register_builtin(registry, TYPE_FLOAT, "float", 4, 4);
    registry->builtins.bool_type = register_builtin(registry, TYPE_BOOL, "bool", 1, 1);

    registry->builtins.byte_type = register_builtin(registry, TYPE_BYTE, "byte", 1, 1);

    // A struct in its layout and a builtin in its semantics: the fields are
    // where its size, its alignment and what it owns all come from, while
    // comparison, literals and the borrow spelling stay the kind's own.
    registry->builtins.string_type = string_builtin_create(registry);

    // The characters, which no slot holds: a 'ref str' is what names them, and
    // that reference is what carries how many there are. Zero-width because
    // nothing is ever laid out for it.
    Type *str = register_builtin(registry, TYPE_STR, "str", 0, 1);

    // How far the characters run is not in their type, so no slot reserves room
    // for one and a reference to them carries that count.
    str->sized = false;
    str->metadata = TYPE_META_LENGTH;
    str->owner = registry->builtins.string_type;

    registry->builtins.str_type = str;

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
    registry->nominals = nominal_key_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->methods = method_key_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->applications =
        type_app_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->arena = arena;
    registry->strings = strings;
    type_registry_register_builtins(registry);

    return registry;
}

void type_registry_destroy(TypeRegistry *registry) {
    type_app_map_destroy(registry->applications);
    method_key_destroy(registry->methods);
    nominal_key_destroy(registry->nominals);
}

Type *type_registry_declare_struct(TypeRegistry *registry, const Scope *scope, String *name,
                                   size_t max_fields) {
    Type *type = type_struct_create(registry->arena, name, max_fields);

    // Interned as soon as its name is bound, so that every field naming it --
    // in this declaration or in one further down the file -- finds this entry
    // rather than building a second.
    nominal_key_insert(registry->nominals, (NominalKey){.scope = scope, .name = name}, type);

    return type;
}

TypeHandle type_registry_find_builtin(TypeRegistry *registry, String *name) {
    return type_registry_find_struct(registry, NULL, name);
}

TypeHandle type_registry_find_struct(TypeRegistry *registry, const Scope *scope, String *name) {
    Type **found = nominal_key_lookup(registry->nominals, (NominalKey){.scope = scope, .name = name});

    return found ? *found : NULL;
}

bool type_registry_add_method(TypeRegistry *registry, TypeHandle type, String *name, Symbol *method) {
    if (type_registry_find_method(registry, type, name)) {
        return false;
    }

    method_key_insert(registry->methods, (MethodKey){.type = type, .name = name}, method);
    return true;
}

Symbol *type_registry_find_method(TypeRegistry *registry, TypeHandle type, const String *name) {
    if (!type) {
        return NULL;
    }

    Symbol **found = method_key_lookup(registry->methods, (MethodKey){.type = type, .name = name});

    if (found) {
        return *found;
    }

    // A type sharing another's identity reads its set: a borrowed string reaches
    // an owning one's, and every 'Array T,N' reaches the bare 'Array's. Its own
    // is consulted first, since what the two do not share is what tells them
    // apart.
    //
    // Finding a method this way is not yet a call that resolves: the owner's
    // methods declare an owning receiver, which a borrow does not satisfy. So
    // 'clone' is found from a borrow and then refused where the receiver is
    // reconciled.
    return type_registry_find_method(registry, type->owner, name);
}

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

    type_app_map_insert(registry->applications, app, type);
}

TypeHandle type_registry_array_of(TypeRegistry *registry, TypeHandle element, int32_t length) {
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

    type->array.element = element;
    type->array.length = length;

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

    object_select_drop(registry->arena, type);

    return type;
}

// The three built-in one-argument constructors, which differ only in the kind
// the result carries. Written once because everything else about building one
// -- the width, the alignment, the pointee -- is the same for all of them.
static TypeHandle indirect_to(TypeRegistry *registry, TypeCtor ctor, TypeKind kind, TypeHandle inner) {
    TypeArg args[] = {{.kind = TYPE_ARG_TYPE, .type = inner}};

    TypeApp app = {.ctor = ctor, .decl = NULL, .args = args, .arg_count = 1};

    Type **existing = application_lookup(registry, app);
    if (existing) {
        return *existing;
    }

    // No name: an indirection is structural, so its printable form is derived
    // from the inner when a diagnostic asks. See Type::name.
    Type *type = type_create(registry->arena, kind, NULL);

    // An address to the payload, so a stack pointer and a heap one are
    // byte-identical; only the type says which is which. A 'ref T' is the same
    // address -- what differs is who frees the inner.
    //
    // Plus whatever a reference to this pointee has to carry: a run of
    // characters is bounded by a count rather than by its type, so a reference
    // to one is an address and that count. The pointee decides, which is what
    // keeps a reference's width from disagreeing with what it names.
    type->indirect.pointee = inner;

    type->size = sizeof(void *);
    type->alignment = _Alignof(void *);

    if (type_metadata_of(inner) == TYPE_META_LENGTH) {
        type->size += sizeof(int32_t);
        type->size = (type->size + type->alignment - 1) & ~(type->alignment - 1);
    }

    // Interned before the drop is selected, so a pointee reaching back here --
    // a struct holding a pointer to its own type -- finds this entry rather
    // than building a second.
    application_insert(registry, app, type);

    object_select_drop(registry->arena, type);

    return type;
}

TypeHandle type_registry_box_to(TypeRegistry *registry, TypeHandle inner) {
    return indirect_to(registry, TYPE_CTOR_BOX, TYPE_BOX, inner);
}

TypeHandle type_registry_ref_to(TypeRegistry *registry, TypeHandle inner) {
    return indirect_to(registry, TYPE_CTOR_REF, TYPE_REF, inner);
}

TypeHandle type_registry_ptr_to(TypeRegistry *registry, TypeHandle pointee) {
    return indirect_to(registry, TYPE_CTOR_PTR, TYPE_PTR, pointee);
}

TypeHandle type_registry_error_type(TypeRegistry *registry) { return registry->builtins.error_type; }

TypeHandle type_registry_get_builtin(TypeRegistry *registry, TypeKind kind) {
    switch (kind) {
    case TYPE_INT:
        return registry->builtins.int_type;
    case TYPE_FLOAT:
        return registry->builtins.float_type;
    case TYPE_BOOL:
        return registry->builtins.bool_type;
    case TYPE_BYTE:
        return registry->builtins.byte_type;
    case TYPE_STRING:
        return registry->builtins.string_type;
    default:
        break;
    }

    assert(0 && "type is not a builtin type");
    abort();
}
