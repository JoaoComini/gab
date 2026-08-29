#include "type_registry_internal.h"

#include "symbol_table.h"

#include "arena.h"
#include "object.h"
#include "string/string.h"
#include "type_internal.h"
#include "util/align.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// What a NULL type answers, so a caller reading a width off nothing reads zero
// rather than dereferencing. Zero-width and aligned to one, which is what a
// type nothing is laid out for is.
static const TypeLayout empty_layout = {.size = 0, .alignment = 1};

static Type *register_builtin(TypeRegistry *registry, TypeKind kind, String *name) {
    return type_create(registry->arena, kind, name);
}

// The types the language is written in terms of, interned before anything else
// can name one. Called by type_registry_create alone: a registry without them is
// one where 'let n: int = 1 + 2' has no int to answer with.
static void register_primitives(TypeRegistry *registry, const TypePrimitiveNames *names) {
    registry->primitives.int_type = register_builtin(registry, TYPE_INT, names->int_name);
    registry->primitives.float_type = register_builtin(registry, TYPE_FLOAT, names->float_name);
    registry->primitives.bool_type = register_builtin(registry, TYPE_BOOL, names->bool_name);

    registry->primitives.byte_type = register_builtin(registry, TYPE_BYTE, names->byte_name);

    // The characters, which no slot holds: a 'ref str' is what names them, and
    // that reference is what carries how many there are. Zero-width because
    // nothing is ever laid out for it, and both of those follow from the kind
    // rather than being set here: see type_is_sized and type_metadata_of.
    //
    // Before the string that lends it, which registers what it derefs to as
    // part of saying what it is.
    registry->primitives.str_type = register_builtin(registry, TYPE_STR, names->str_name);

    // The bare name every '[T; N]' is interned under. Sized as the header the
    // elements make it, so that a diagnostic naming it says something true even
    // though no slot ever holds this type itself.
    registry->primitives.array_type = register_builtin(registry, TYPE_ARRAY, names->array_name);

    // Poison type. Deliberately never given a name in any scope: no script can
    // name it, it only arises from a failed resolution.
    registry->primitives.error_type = register_builtin(registry, TYPE_ERROR, names->error_name);
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

    // A block carries the capacity it was allocated at and how far into it
    // anything was written, whatever it points to: both are the block's own
    // facts rather than something read off the element, which is what lets it
    // be freed without asking anything else.
    //
    // Two counts where a reference carries one, and no wider for it: the second
    // rides in what the address's alignment already padded out. Both counts are
    // named here rather than one, so the arithmetic says what the block holds
    // even where the padding makes the two spellings the same width.
    if (type->kind == TYPE_BLOCK) {
        *size = align_up(*size + 2 * sizeof(int32_t), *alignment);
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

void type_registry_set_deref(TypeRegistry *registry, const Type *from, const Type *to, const LentPart *parts,
                             size_t part_count) {
    assert(from && to && "a deref relates two types");
    assert(part_count <= GAB_MAX_LENT_PARTS && "a reference is built from at most GAB_MAX_LENT_PARTS parts");
    assert(!deref_key_lookup(registry->derefs, from) && "a type derefs to one thing");

    Deref *deref = arena_alloc(registry->arena, sizeof(Deref));

    *deref = (Deref){.to = to, .part_count = part_count};

    for (size_t i = 0; i < part_count; i++) {
        deref->parts[i] = parts[i];
    }

    deref_key_insert(registry->derefs, from, deref);
}

const Deref *type_registry_deref(TypeRegistry *registry, const Type *type) {
    if (!type) {
        return NULL;
    }

    const Deref **found = deref_key_lookup(registry->derefs, type);

    return found ? *found : NULL;
}

const Type *type_registry_deref_of(TypeRegistry *registry, const Type *type) {
    const Deref *deref = type_registry_deref(registry, type);

    return deref ? deref->to : NULL;
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

TypeRegistry *type_registry_create(Arena *arena, const TypePrimitiveNames *names) {
    TypeRegistry *registry = arena_alloc(arena, sizeof(TypeRegistry));
    registry->drops = drop_key_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->derefs = deref_key_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->layouts = layout_key_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->methods = method_key_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->applications =
        type_app_map_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->arena = arena;
    register_primitives(registry, names);

    return registry;
}

void type_registry_destroy(TypeRegistry *registry) {
    type_app_map_destroy(registry->applications);
    method_key_destroy(registry->methods);
    drop_key_destroy(registry->drops);
    deref_key_destroy(registry->derefs);
}

Type *type_registry_open_struct(TypeRegistry *registry, String *name, size_t max_fields) {
    return type_struct_create(registry->arena, name, max_fields);
}

void type_registry_complete(TypeRegistry *registry, Type *type) {
    type_registry_layout_of(registry, type);
    type_registry_drop_of(registry, type);
}

const Type *type_registry_declare(TypeRegistry *registry, const TypeDecl *decl) {
    assert(decl && decl->name && "a declared type is found by name");
    assert(!(decl->fields && decl->generic) && "a type is a struct or a generic declaration, not both");
    assert((decl->derefs_to != NULL) == (decl->lent_part_count > 0) &&
           "a deref and the parts naming it are one statement");

    Type *type = decl->generic ? type_create(registry->arena, TYPE_STRUCT, decl->name)
                               : type_registry_open_struct(registry, decl->name, decl->field_count);

    for (size_t i = 0; i < decl->field_count; i++) {
        type_add_field(type, decl->fields[i].name, decl->fields[i].type);
    }

    type->generic = decl->generic;

    // Settled here rather than on first demand, so the type the provider gets
    // back is finished: what it owns follows from the fields just added.
    //
    // A generic declaration is not laid out -- a width follows from an
    // instantiation's fields, and a parameter has none -- so only what it owns
    // is settled.
    if (decl->generic) {
        type_registry_drop_of(registry, type);
    } else {
        type_registry_complete(registry, type);
    }

    if (decl->derefs_to) {
        type_registry_set_deref(registry, type, decl->derefs_to, decl->lent_parts, decl->lent_part_count);
    }

    return type;
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
        .decl = registry->primitives.array_type,
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
    Type *type = type_create(registry->arena, TYPE_ARRAY, registry->primitives.array_type->name);

    type->array.element = element;
    type->array.length = length;

    // The declaration this instantiates, which is where its methods are
    // declared while none of them reads the element.
    type->decl = registry->primitives.array_type;

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
//
// The Symbol is built here because everything it needs is here: the substituted
// signature, and the arena types already live in. What it does not get is a
// body index -- that is a table a unit owns and linking rebases, so it stays
// SYMBOL_FUNC_NO_BODY until codegen reserves a slot for it.
static void declare_generic_methods(TypeRegistry *registry, Type *type, const GenericDecl *generic,
                                    const Type *const *args) {
    for (size_t i = 0; i < generic->method_count; i++) {
        const GenericMethod *method = &generic->methods[i];

        Symbol *symbol = arena_alloc(registry->arena, sizeof(Symbol));

        // The receiver is parameter zero, as it is for every method: what a
        // call writes follows it.
        const Type **params = arena_alloc(registry->arena, (method->param_count + 1) * sizeof(const Type *));

        params[0] = generic_field_type(registry, &method->receiver, args, type);

        for (size_t p = 0; p < method->param_count; p++) {
            params[p + 1] = generic_field_type(registry, &method->params[p], args, type);
        }

        *symbol = (Symbol){
            .kind = SYMBOL_FUNC,
            .func =
                {
                    .name = method->name,
                    .return_type = generic_field_type(registry, &method->result, args, type),
                    .params = params,
                    .param_count = method->param_count + 1,
                    .is_extern = true,
                    .body = method->body,
                    .func_index = SYMBOL_FUNC_NO_BODY,
                },
        };

        type_registry_add_method(registry, type, method->name, symbol);
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

    type_registry_drop_of(registry, type);

    declare_generic_methods(registry, type, generic, args);

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

const Type *type_registry_error_type(TypeRegistry *registry) { return registry->primitives.error_type; }

const Type *type_registry_get_primitive(TypeRegistry *registry, TypeKind kind) {
    switch (kind) {
    case TYPE_INT:
        return registry->primitives.int_type;
    case TYPE_FLOAT:
        return registry->primitives.float_type;
    case TYPE_BOOL:
        return registry->primitives.bool_type;
    case TYPE_BYTE:
        return registry->primitives.byte_type;
    case TYPE_STR:
        return registry->primitives.str_type;

    // The bare name every '[T; N]' is an application of, which is what a spec
    // resolves before its element is applied.
    case TYPE_ARRAY:
        return registry->primitives.array_type;
    case TYPE_ERROR:
        return registry->primitives.error_type;
    default:
        break;
    }

    assert(0 && "type is not a builtin type");
    abort();
}

TypePrimitiveNames type_primitive_names(StringPool *strings) {
    return (TypePrimitiveNames){
        .int_name = string_from_cstr(strings, "int"),
        .float_name = string_from_cstr(strings, "float"),
        .bool_name = string_from_cstr(strings, "bool"),

        // Not spellable in the language: it exists so that a pointer to
        // characters carries a stride. Named all the same, so that a diagnostic
        // can print it.
        .byte_name = string_from_cstr(strings, "byte"),
        .str_name = string_from_cstr(strings, "str"),
        .array_name = string_from_cstr(strings, "Array"),

        // The poison type, never bound in any scope: no script can name it.
        .error_name = string_from_cstr(strings, "<error>"),
    };
}
