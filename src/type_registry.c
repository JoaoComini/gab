#include "type_registry.h"

#include "arena.h"
#include "builtin/builtin.h"
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

// The 'String' type: a block of characters and how many of them are live.
//
// The same two fields a vector has, and freed by the same walk -- a string is
// what a 'Vec<byte>' is, with characters for elements. What it does not share
// is how one is written: a literal and a comparison are about the characters
// themselves, and no struct shape implies them.
static Type *string_builtin_create(TypeRegistry *registry) {
    Type *type = type_struct_create(registry->arena, string_from_cstr(registry->strings, "String"), 2);

    // The block owns the characters and carries the capacity it was taken at,
    // so freeing it asks nothing else -- and the room past the live ones is
    // what lets a string grow without reallocating on every push.
    const Type *characters = type_registry_block_of(registry, registry->builtins.byte_type);

    type_add_field(type, string_from_cstr(registry->strings, "data"), characters);
    type_add_field(type, string_from_cstr(registry->strings, "length"), registry->builtins.int_type);

    // What separates it from a 'Vec<byte>', which is laid out identically: the
    // characters are text, so this is what lends a 'ref str' and what '=='
    // reads.
    type->holds_characters = true;

    // How a string frees, which its shape does not say -- the same composition
    // a vector's takes, since what differs between them is the element rather
    // than how one is freed.
    type_registry_set_drop(registry, type, string_build_drop(registry, type));

    return type;
}

// The 'Vec' declaration: a block of the element it is given, and a count of how
// many of that block have been written.
//
// A declaration, so no width is settled here -- what a vector is laid out as
// follows from the element, and that is not known until one is applied.
static Type *vec_decl_create(TypeRegistry *registry) {
    Type *type = register_builtin(registry, TYPE_STRUCT, "Vec");

    GenericField *fields = arena_alloc(registry->arena, 2 * sizeof(GenericField));

    // The block owns the memory and carries the capacity it was taken at, so
    // freeing it asks nothing else.
    fields[0] = (GenericField){
        .name = string_from_cstr(registry->strings, "data"),
        .from = GENERIC_FROM_PARAM,
        .param = 0,
        .ctor = TYPE_CTOR_BLOCK,
    };

    // How far into that block anything has been written. Beside the block
    // rather than in it, because a capacity and a length answer different
    // questions and only one of them is the memory's own.
    fields[1] = (GenericField){
        .name = string_from_cstr(registry->strings, "length"),
        .fixed = registry->builtins.int_type,
    };

    GenericDecl *generic = arena_alloc(registry->arena, sizeof(GenericDecl));

    *generic = (GenericDecl){
        .param_count = 1,
        .fields = fields,
        .field_count = 2,

        // How a vector frees, which its shape does not say: see vec_build_drop.
        .build_drop = vec_build_drop,
    };

    type->generic = generic;

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
    // nothing is ever laid out for it, and both of those follow from the kind
    // rather than being set here: see type_is_sized and type_metadata_of.
    registry->builtins.str_type = register_builtin(registry, TYPE_STR, "str");

    // The VM and the host both read these two fields as GabStringValue, so the
    // computed layout and the C struct are two statements of one thing.
    const TypeLayout *string_layout = type_registry_layout_of(registry, registry->builtins.string_type);

    assert(string_layout->size == sizeof(GabStringValue));
    assert(string_layout->alignment == _Alignof(GabStringValue));
    (void)string_layout;

    // And a reference to those characters is what a C body reads one as. The
    // width is computed from what the reference carries rather than from this
    // struct, so the two agreeing is a fact to check rather than one to assume.
    const TypeLayout *str_ref_layout =
        type_registry_layout_of(registry, type_registry_ref_to(registry, registry->builtins.str_type));

    assert(str_ref_layout->size == sizeof(GabStrRef));
    assert(str_ref_layout->alignment == _Alignof(GabStrRef));
    (void)str_ref_layout;

    // The bare name every '[T; N]' is interned under. Sized as the header the
    // elements make it, so that a diagnostic naming it says something true even
    // though no slot ever holds this type itself.
    registry->builtins.array_type = register_builtin(registry, TYPE_ARRAY, "Array");

    // 'Vec', the bare name every 'Vec<T>' is instantiated from. A declaration
    // rather than a type: it says what its instantiations hold -- a block of
    // the parameter, and how many of that block are live -- without naming an
    // element, which is what makes it generic.
    registry->builtins.vec_type = vec_decl_create(registry);

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

// A run of elements, laid out exactly as a C '[T; N]' is: the elements live in
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

    // A block carries the capacity it was allocated at, whatever it points to:
    // the count is the block's own fact rather than something read off the
    // element, which is what lets it be freed without asking anything else.
    if (type->kind == TYPE_BLOCK) {
        *size = align_up(*size + sizeof(int32_t), *alignment);
        return;
    }

    // A pointee whose bounds are not in its type is named by more than an
    // address: how many elements it runs to lives with the value, so the
    // reference carries that count beside the address.
    //
    // The count's width, not any one reference's: a slice answers
    // TYPE_META_LENGTH for the reason characters do, and reaches here for the
    // same width. What the host reads such a reference as -- 'GabStrRef' for
    // characters -- is asserted against this rather than being what it is
    // computed from, so a second such pointee needs no arm here.
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
        case TYPE_BLOCK:
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

void type_registry_set_drop(TypeRegistry *registry, const Type *type, const DropPlan *plan) {
    assert(!drop_key_lookup(registry->drops, type) &&
           "a supplied drop must be given before the type's plan is first asked for");

    drop_key_insert(registry->drops, type, plan);
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
    registry->install_method = NULL;
    registry->install_ctx = NULL;
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

    // An instantiation answers what its declaration declares: every
    // '[T; N]' reaches the bare 'Array's set. Its own is consulted first,
    // since what the two do not share is what tells them apart.
    return type_registry_find_method(registry, type->decl, name);
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
    // and its alignment is one element's. No header, no block: an '[T; N]'
    // is laid out exactly as a C '[T; N]' is, which is what lets a host lay one
    // out with sizeof.
    Type *type = type_create(registry->arena, TYPE_ARRAY, registry->builtins.array_type->name);

    type->array.element = element;
    type->array.length = length;

    // The declaration this instantiates, which is where its methods are
    // declared while none of them reads the element.
    type->decl = registry->builtins.array_type;

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

// What one field of a declaration comes to, once the parameters are known. A
// field naming no parameter is already its own answer; one that does is that
// parameter with the field's constructor applied.
static const Type *generic_field_type(TypeRegistry *registry, const GenericField *field,
                                      const Type *const *args, const Type *self) {
    if (field->fixed) {
        return field->fixed;
    }

    // A method that returns nothing, which is what a NULL type is everywhere.
    if (field->from == GENERIC_FROM_NOTHING) {
        return NULL;
    }

    const Type *argument = field->from == GENERIC_FROM_SELF ? self : args[field->param];

    switch (field->ctor) {
    case TYPE_CTOR_BOX:
        return type_registry_box_to(registry, argument);
    case TYPE_CTOR_REF:
        return type_registry_ref_to(registry, argument);
    case TYPE_CTOR_PTR:
        return type_registry_ptr_to(registry, argument);
    case TYPE_CTOR_BLOCK:
        return type_registry_block_of(registry, argument);

    // The parameter itself, held by value. A nominal constructor is what a
    // declaration naming another generic would be, which nothing declares yet.
    case TYPE_CTOR_NOMINAL:
        break;
    }

    return argument;
}

// Declares what this instantiation answers to, with the parameters filled in.
// Nothing at all where no installer was registered: a compile with no VM has no
// extern table to number a body in, and nothing in it can call one.
static void install_generic_methods(TypeRegistry *registry, const Type *type, const GenericDecl *generic,
                                    const Type *const *args) {
    if (!registry->install_method) {
        return;
    }

    for (size_t i = 0; i < generic->method_count; i++) {
        const GenericMethod *method = &generic->methods[i];

        // The receiver is parameter zero, as it is for every method: what a
        // call writes follows it.
        const Type *signature[GAB_MAX_METHOD_PARAMS];

        signature[0] = generic_field_type(registry, &method->receiver, args, type);

        for (size_t p = 0; p < method->param_count; p++) {
            signature[p + 1] = generic_field_type(registry, &method->params[p], args, type);
        }

        registry->install_method(registry->install_ctx, type, method->name, method->body,
                                 generic_field_type(registry, &method->result, args, type), signature,
                                 method->param_count + 1);
    }
}

const Type *type_registry_instantiate(TypeRegistry *registry, const Type *decl, const Type *const *args,
                                      size_t arg_count) {
    assert(decl && decl->generic && "only a generic declaration is instantiated");
    assert(arg_count == decl->generic->param_count && "an instantiation was given the wrong argument count");

    TypeArg key_args[GAB_MAX_TYPE_PARAMS];

    for (size_t i = 0; i < arg_count; i++) {
        key_args[i] = (TypeArg){.kind = TYPE_ARG_TYPE, .type = args[i]};
    }

    TypeApp app = {
        .ctor = TYPE_CTOR_NOMINAL,
        .decl = decl,
        .args = key_args,
        .arg_count = arg_count,
    };

    Type **existing = application_lookup(registry, app);
    if (existing) {
        return *existing;
    }

    // A struct in every way that is laid out: the fields are where its width,
    // its alignment and what it owns all come from. What the declaration adds
    // is that they were written in terms of a parameter.
    Type *type = type_create(registry->arena, TYPE_STRUCT, decl->name);

    const GenericDecl *generic = decl->generic;

    TypeField *fields = arena_alloc(registry->arena, generic->field_count * sizeof(TypeField));

    for (size_t i = 0; i < generic->field_count; i++) {
        fields[i] = (TypeField){
            .name = generic->fields[i].name,
            .type = generic_field_type(registry, &generic->fields[i], args, type),
        };
    }

    type->record.fields = fields;
    type->record.field_count = generic->field_count;

    // The declaration this instantiates, which is where its methods are
    // declared and where a substituted signature is read from.
    type->decl = decl;

    // Interned before the drop is selected, so a field reaching back here finds
    // this entry rather than building a second.
    application_insert(registry, app, type);

    // What the declaration says frees an instantiation, composed against the
    // fields this one was laid out with. Nothing here knows what that plan
    // walks: the declaration does, and hands back what it built.
    if (generic->build_drop) {
        type_registry_set_drop(registry, type, generic->build_drop(registry, type));
    }

    type_registry_drop_of(registry, type);

    install_generic_methods(registry, type, generic, args);

    return type;
}

const Type *type_registry_block_of(TypeRegistry *registry, const Type *element) {
    return indirect_to(registry, TYPE_CTOR_BLOCK, TYPE_BLOCK, element);
}

const Type *type_registry_ref_to(TypeRegistry *registry, const Type *inner) {
    return indirect_to(registry, TYPE_CTOR_REF, TYPE_REF, inner);
}

const Type *type_registry_ptr_to(TypeRegistry *registry, const Type *pointee) {
    return indirect_to(registry, TYPE_CTOR_PTR, TYPE_PTR, pointee);
}

Arena *type_registry_arena(TypeRegistry *registry) { return registry->arena; }

const Type *type_registry_string(TypeRegistry *registry) { return registry->builtins.string_type; }

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
    default:
        break;
    }

    assert(0 && "type is not a builtin type");
    abort();
}
