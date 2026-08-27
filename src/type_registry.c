#include "type_registry.h"

#include "arena.h"
#include "object.h"
#include "string/string.h"
#include "type.h"
#include "util/align.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// What a NULL type answers, so a caller reading a width off nothing reads zero
// rather than dereferencing. Zero-width and aligned to one, which is what a
// type nothing is laid out for is.
static const TypeLayout empty_layout = {.size = 0, .alignment = 1};

static Type *register_builtin(TypeRegistry *registry, TypeKind kind, const char *name) {
    return type_create(registry->arena, kind, string_from_cstr(registry->strings, name));
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
    const Type *characters = type_registry_ptr_to(registry, registry->builtins.byte_type);

    type_add_field(type, string_from_cstr(registry->strings, "data"), characters);
    type_add_field(type, string_from_cstr(registry->strings, "length"), registry->builtins.int_type);

    // A string owns its characters, always: what borrows them is a 'ref str',
    // which owns nothing because no reference does. So this is not a question
    // the type has to be asked -- it is what being a String means.
    type_registry_drop_of(registry, type);

    return type;
}

void type_registry_register_builtins(TypeRegistry *registry) {
    registry->builtins.int_type = register_builtin(registry, TYPE_INT, "int");
    registry->builtins.float_type = register_builtin(registry, TYPE_FLOAT, "float");
    registry->builtins.bool_type = register_builtin(registry, TYPE_BOOL, "bool");

    registry->builtins.byte_type = register_builtin(registry, TYPE_BYTE, "byte");

    // A struct in its layout and a builtin in its semantics: the fields are
    // where its size, its alignment and what it owns all come from, while
    // comparison, literals and the borrow spelling stay the kind's own.
    registry->builtins.string_type = string_builtin_create(registry);

    // The characters, which no slot holds: a 'ref str' is what names them, and
    // that reference is what carries how many there are. Zero-width because
    // nothing is ever laid out for it.
    Type *str = register_builtin(registry, TYPE_STR, "str");

    // How far the characters run is not in their type, so no slot reserves room
    // for one and a reference to them carries that count.
    str->sized = false;
    str->metadata = TYPE_META_LENGTH;
    str->owner = registry->builtins.string_type;

    registry->builtins.str_type = str;

    // The VM and the host both read these two fields as GabStringValue, so the
    // computed layout and the C struct are two statements of one thing.
    const TypeLayout *string_layout = type_registry_layout_of(registry, registry->builtins.string_type);

    assert(string_layout->size == sizeof(GabStringValue));
    assert(string_layout->alignment == _Alignof(GabStringValue));
    (void)string_layout;

    // The bare name every 'Array T' is interned under. Sized as the header the
    // elements make it, so that a diagnostic naming it says something true even
    // though no slot ever holds this type itself.
    registry->builtins.array_type = register_builtin(registry, TYPE_ARRAY, "Array");

    // Poison type. Deliberately never given a name in any scope: no script can
    // name it, it only arises from a failed resolution.
    registry->builtins.error_type = register_builtin(registry, TYPE_ERROR, "<error>");
}

// The width and alignment of a kind whose layout is a fact about the machine
// rather than about what the type is built of: a scalar, an address, or a type
// nothing is ever laid out for.
//
// Returns false for the kinds whose layout is derived -- a struct from its
// fields, an array from its element -- which is what the caller then does.
static bool layout_of_scalar(TypeKind kind, size_t *size, size_t *alignment) {
    switch (kind) {
    case TYPE_INT:
        *size = sizeof(int32_t);
        *alignment = _Alignof(int32_t);
        return true;

    case TYPE_FLOAT:
        *size = sizeof(float);
        *alignment = _Alignof(float);
        return true;

    case TYPE_BOOL:
        *size = sizeof(bool);
        *alignment = _Alignof(bool);
        return true;

    case TYPE_BYTE:
        *size = 1;
        *alignment = 1;
        return true;

    // Never held: the characters a 'ref str' names, the bare 'Array' before an
    // element is applied, and the type a failed resolution yields. Each is
    // zero-width because no slot is ever reserved for one.
    case TYPE_STR:
    case TYPE_ERROR:
        *size = 0;
        *alignment = 1;
        return true;

    default:
        return false;
    }
}

// A run of elements, laid out exactly as a C 'T[N]' is: the elements live in
// the array itself, so its width is the run of them and its alignment is one
// element's.
static void layout_of_array(TypeRegistry *registry, const Type *type, size_t *size, size_t *alignment) {
    const TypeLayout *element = type_registry_layout_of(registry, type_array_element(type));

    *size = element->size * (size_t)type_array_length(type);
    *alignment = element->alignment;
}

// An address to the payload, so a stack pointer and a heap one are
// byte-identical; only the type says which is which.
//
// Plus whatever a reference to the pointee has to carry: a run of characters is
// bounded by a count rather than by its type, so a reference to one is an
// address and that count. The pointee decides, which is what keeps a
// reference's width from disagreeing with what it names.
static void layout_of_indirect(const Type *type, size_t *size, size_t *alignment) {
    *size = sizeof(void *);
    *alignment = _Alignof(void *);

    if (type_metadata_of(type_pointee(type)) == TYPE_META_LENGTH) {
        *size = align_up(*size + sizeof(int32_t), *alignment);
    }
}

// A record's fields, each placed at the first offset its own alignment allows.
// The whole is aligned to the widest of them and padded out to a multiple of
// that, which is what makes an array of the type place every element right.
static void layout_of_fields(TypeRegistry *registry, const Type *type, size_t *offsets, size_t *size,
                             size_t *alignment) {
    size_t offset = 0;

    *alignment = 1;

    for (size_t i = 0; i < type_field_count(type); i++) {
        const TypeLayout *field = type_registry_layout_of(registry, type_fields(type)[i].type);

        offset = align_up(offset, field->alignment);
        offsets[i] = offset;
        offset += field->size;

        if (field->alignment > *alignment) {
            *alignment = field->alignment;
        }
    }

    *size = align_up(offset, *alignment);
}

const TypeLayout *type_registry_layout_of(TypeRegistry *registry, const Type *type) {
    if (!type) {
        return &empty_layout;
    }

    const TypeLayout **found = layout_key_lookup(registry->layouts, type);

    if (found) {
        return *found;
    }

    TypeLayout *layout = arena_alloc(registry->arena, sizeof(TypeLayout));

    *layout = (TypeLayout){.size = 0, .alignment = 1};

    if (!layout_of_scalar(type->kind, &layout->size, &layout->alignment)) {
        switch (type->kind) {
        case TYPE_ARRAY:
            layout_of_array(registry, type, &layout->size, &layout->alignment);
            break;

        case TYPE_BOX:
        case TYPE_REF:
        case TYPE_PTR:
            layout_of_indirect(type, &layout->size, &layout->alignment);
            break;

        default: {
            size_t count = type_field_count(type);
            size_t *offsets = count > 0 ? arena_alloc(registry->arena, count * sizeof(size_t)) : NULL;

            layout_of_fields(registry, type, offsets, &layout->size, &layout->alignment);

            layout->offsets = offsets;
            layout->offset_count = count;
            break;
        }
        }
    }

    layout_key_insert(registry->layouts, type, layout);

    return layout;
}

size_t type_registry_size_of(TypeRegistry *registry, const Type *type) {
    return type_registry_layout_of(registry, type)->size;
}

size_t type_registry_align_of(TypeRegistry *registry, const Type *type) {
    return type_registry_layout_of(registry, type)->alignment;
}

const DropPlan *type_registry_drop_of(TypeRegistry *registry, const Type *type) {
    if (!type) {
        return NULL;
    }

    const DropPlan **found = drop_key_lookup(registry->drops, type);

    if (found) {
        return *found;
    }

    // No guard against reaching this type again on the way down: an
    // indirection's plan carries no inner, so the recursion only ever descends
    // through fields and array elements -- and a type held by value inside
    // itself was refused as a containment cycle long before here.
    const DropPlan *plan = object_build_drop(registry->arena, registry, type);

    drop_key_insert(registry->drops, type, plan);

    return plan;
}

TypeRegistry *type_registry_create(Arena *arena, StringPool *strings) {
    TypeRegistry *registry = arena_alloc(arena, sizeof(TypeRegistry));
    registry->drops = drop_key_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->layouts = layout_key_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
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
    drop_key_destroy(registry->drops);
}

Type *type_registry_declare_struct(TypeRegistry *registry, String *name, size_t max_fields) {
    return type_struct_create(registry->arena, name, max_fields);
}

bool type_registry_add_method(TypeRegistry *registry, const Type *type, String *name, Symbol *method) {
    if (type_registry_find_method(registry, type, name)) {
        return false;
    }

    method_key_insert(registry->methods, (MethodKey){.type = type, .name = name}, method);
    return true;
}

Symbol *type_registry_find_method(TypeRegistry *registry, const Type *type, const String *name) {
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

const Type *type_registry_array_of(TypeRegistry *registry, const Type *element, int32_t length) {
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

    // Every array answers the same methods, which do not depend on the element.
    // Reached through 'owner' rather than copied into each set, the way a 'str'
    // reaches a 'String's.
    type->owner = registry->builtins.array_type;

    // Interned before the drop is selected, so a recursive element -- an array
    // of a struct holding one -- finds this entry rather than building a
    // second.
    application_insert(registry, app, type);

    type_registry_drop_of(registry, type);

    return type;
}

// The three built-in one-argument constructors, which differ only in the kind
// the result carries. Written once because everything else about building one
// -- the width, the alignment, the pointee -- is the same for all of them.
static const Type *indirect_to(TypeRegistry *registry, TypeCtor ctor, TypeKind kind, const Type *inner) {
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

    // Interned before the drop is selected, so a pointee reaching back here --
    // a struct holding a pointer to its own type -- finds this entry rather
    // than building a second.
    application_insert(registry, app, type);

    type_registry_drop_of(registry, type);

    return type;
}

const Type *type_registry_box_to(TypeRegistry *registry, const Type *inner) {
    return indirect_to(registry, TYPE_CTOR_BOX, TYPE_BOX, inner);
}

const Type *type_registry_ref_to(TypeRegistry *registry, const Type *inner) {
    return indirect_to(registry, TYPE_CTOR_REF, TYPE_REF, inner);
}

const Type *type_registry_ptr_to(TypeRegistry *registry, const Type *pointee) {
    return indirect_to(registry, TYPE_CTOR_PTR, TYPE_PTR, pointee);
}

const Type *type_registry_error_type(TypeRegistry *registry) { return registry->builtins.error_type; }

const Type *type_registry_get_builtin(TypeRegistry *registry, TypeKind kind) {
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
