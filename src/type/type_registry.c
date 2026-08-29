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

// A primitive is declared by the language rather than by a unit, but it is
// declared: what it answers to hangs on its declaration exactly as a struct's
// does, so a method set has one kind of owner rather than two.
//
// Taking no parameters, which is what makes the bare name a type: 'int' is its
// own instantiation, as every declaration applying none is.
// A primitive is the type it names, declared by nothing: no declaration stands
// behind it, so 'decl' stays NULL and nothing instantiates it. That is rustc's
// position too -- Res::PrimTy is its own resolution kind, apart from Res::Def.
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
    // An array takes an element and a length, so its declaration takes two
    // arguments. It declares no fields: what an array holds is a run of its
    // element, which the type carries rather than a field list.
    TypeDef *array_def = arena_alloc(registry->arena, sizeof(TypeDef));

    *array_def = (TypeDef){.name = names->array_name, .param_count = 2, .shape = TYPE_SHAPE_ARRAY};

    registry->primitives.array_def = array_def;

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

    // Interned on demand, so nothing stands here until a declaration asks for
    // one. The arena hands back whatever it held, and this is what a lookup
    // tests against.
    memset(registry->params, 0, sizeof(registry->params));

    register_primitives(registry, names);

    return registry;
}

void type_registry_destroy(TypeRegistry *registry) {
    type_app_map_destroy(registry->applications);
    method_key_destroy(registry->methods);
    drop_key_destroy(registry->drops);
    deref_key_destroy(registry->derefs);
}

void type_registry_complete(TypeRegistry *registry, const Type *type) {
    type_registry_layout_of(registry, type);
    type_registry_drop_of(registry, type);
}

const Type *type_registry_declare(TypeRegistry *registry, const TypeDecl *decl) {
    assert(decl && decl->def && decl->def->name && "a declared type is found by name");
    assert((decl->derefs_to != NULL) == (decl->lent_part_count > 0) &&
           "a deref and the parts naming it are one statement");

    // Instantiated with no arguments when it takes none, which is what a plain
    // struct is: one path settles both, and a declaration that takes parameters
    // is laid out only once some mention supplies them.
    const Type *type = decl->def->param_count == 0 ? type_registry_apply(registry, decl->def, NULL, 0) : NULL;

    // Its fields were stated with it, so nothing is waiting: a provider names a
    // type it already knows the shape of, where a unit's struct is interned
    // first and laid out once its fields resolve.
    if (type) {
        type_registry_complete(registry, type);
    }

    if (decl->derefs_to) {
        type_registry_set_deref(registry, type, decl->derefs_to, decl->lent_parts, decl->lent_part_count);
    }

    return type;
}

const Type *type_registry_declare_struct(TypeRegistry *registry, String *name, const TypeFieldSpec *fields,
                                         size_t field_count) {
    assert(name && "a declared type is found by name");
    assert((field_count == 0 || fields) && "a struct with fields states them");

    TypeField *owned = field_count ? arena_alloc(registry->arena, field_count * sizeof(TypeField)) : NULL;

    for (size_t i = 0; i < field_count; i++) {
        owned[i] = (TypeField){.name = fields[i].name, .type = fields[i].type};
    }

    // In the registry's arena rather than the caller's frame: the type interned
    // below reads its fields through this declaration for as long as it lives.
    TypeDef *def = arena_alloc(registry->arena, sizeof(TypeDef));

    *def = (TypeDef){.name = name, .fields = owned, .field_count = field_count};

    const TypeDecl decl = {.def = def};

    return type_registry_declare(registry, &decl);
}

bool type_registry_add_method(TypeRegistry *registry, const Type *type, String *name, Symbol *method) {
    assert(type && name && method && "a method is a type, a name and a body");

    // The type's set is asked through the walk that also reaches its
    // declaration's, so a name either already holds is a collision rather than
    // an entry the walk would never reach past the first.
    if (type_registry_find_method(registry, type, name)) {
        return false;
    }

    method_key_insert(registry->methods, (MethodKey){.type = type, .name = name}, method);
    return true;
}

bool type_registry_add_shared_method(TypeRegistry *registry, const TypeDef *def, String *name,
                                     Symbol *method) {
    assert(def && name && method && "a shared method is a declaration, a name and a body");

    MethodKey key = {.def = def, .name = name};

    if (method_key_lookup(registry->methods, key)) {
        return false;
    }

    method_key_insert(registry->methods, key, method);
    return true;
}

const TypeDef *type_registry_array_def(TypeRegistry *registry) { return registry->primitives.array_def; }

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
    if (!type->decl) {
        return NULL;
    }

    found = method_key_lookup(registry->methods, (MethodKey){.def = type->decl, .name = name});

    return found ? *found : NULL;
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

const Type *type_registry_apply(TypeRegistry *registry, const TypeDef *def, const Type *const *args,
                                size_t arg_count) {
    TypeArg type_args[GAB_MAX_TYPE_PARAMS];

    assert(arg_count <= GAB_MAX_TYPE_PARAMS && "a declaration takes no more parameters than this");

    for (size_t i = 0; i < arg_count; i++) {
        type_args[i] = (TypeArg){.kind = TYPE_ARG_TYPE, .type = args[i]};
    }

    return type_registry_instantiate(registry, def, type_args, arg_count);
}

const Type *type_registry_array_of(TypeRegistry *registry, const Type *element, int32_t length) {
    const TypeArg args[] = {
        {.kind = TYPE_ARG_TYPE, .type = element},
        {.kind = TYPE_ARG_CONST, .value = length},
    };

    return type_registry_instantiate(registry, registry->primitives.array_def, args, 2);
}

// The three built-in one-argument constructors, which differ only in the kind
// the result carries. Written once because everything else about building one
// -- the width, the alignment, the pointee -- is the same for all of them.
static const Type *indirect_to(TypeRegistry *registry, TypeCtor ctor, TypeKind kind, const Type *inner) {
    TypeArg args[] = {{.kind = TYPE_ARG_TYPE, .type = inner}};

    TypeApp app = {.ctor = ctor, .def = NULL, .args = args, .arg_count = 1};

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
    type->has_param = type_has_param(inner);

    // Interned before the drop is selected, so a pointee reaching back here --
    // a struct holding a pointer to its own type -- finds this entry rather
    // than building a second.
    application_insert(registry, app, type);

    // A shape written over a parameter is not a type a value ever has: it is
    // what a declaration says, and what substituting it produces is laid out
    // instead. Asking what freeing 'box T' does has no answer until T is known,
    // and a parameter can only be here because a declaration put it there.
    if (!type->has_param) {
        type_registry_drop_of(registry, type);
    }

    return type;
}

const Type *type_registry_box_to(TypeRegistry *registry, const Type *inner) {
    return indirect_to(registry, TYPE_CTOR_BOX, TYPE_BOX, inner);
}

const Type *type_registry_param(TypeRegistry *registry, size_t index) {
    assert(index < GAB_MAX_TYPE_PARAMS && "a declaration takes at most GAB_MAX_TYPE_PARAMS parameters");

    if (!registry->params[index]) {
        Type *type = type_create(registry->arena, TYPE_PARAM, NULL);

        type->param.index = index;

        registry->params[index] = type;
    }

    return registry->params[index];
}

/*
    A type written over a declaration's parameters, with the arguments filled in.

    A fold over the ordinary type tree: a parameter becomes the argument at its
    index, a constructor is rebuilt over its substituted inner, and everything
    else is already its own answer. That last case is what keeps this cheap --
    a type mentioning no parameter is returned untouched rather than rebuilt.

    Rebuilding through the same constructors any other type is built by is the
    point: 'block box T' substitutes because a block of a box is what it is,
    not because a field was allowed to name two constructors.
*/
static const Type *substitute(TypeRegistry *registry, const Type *type, const Type *const *args,
                              size_t arg_count) {
    if (!type_has_param(type)) {
        return type;
    }

    switch (type_kind(type)) {
    case TYPE_PARAM: {
        size_t index = type_param_index(type);

        assert(index < arg_count && "a parameter was substituted with no argument at its index");

        return args[index];
    }

    case TYPE_BOX:
        return type_registry_box_to(registry, substitute(registry, type_pointee(type), args, arg_count));

    case TYPE_REF:
        return type_registry_ref_to(registry, substitute(registry, type_pointee(type), args, arg_count));

    case TYPE_PTR:
        return type_registry_ptr_to(registry, substitute(registry, type_pointee(type), args, arg_count));

    case TYPE_BLOCK:
        return type_registry_block_of(registry, substitute(registry, type_pointee(type), args, arg_count));

    case TYPE_ARRAY:
        return type_registry_array_of(registry,
                                      substitute(registry, type_array_element(type), args, arg_count),
                                      type_array_length(type));

    // An instantiation written over the parameters, which is what a method's
    // receiver is: 'Vec<T>' under these arguments is the 'Vec<int>' being
    // built. Instantiating it again finds the entry inserted before any of
    // this ran rather than building a second.
    case TYPE_STRUCT: {
        assert(type_decl(type) && "a struct mentioning a parameter is an instantiation");

        return type_registry_apply(registry, type_decl(type), args, arg_count);
    }

    default:
        break;
    }

    assert(false && "a type mentioning a parameter is one substitution rebuilds");

    return type;
}

// Declares what this instantiation answers to, with the parameters filled in.
//
// The Symbol is built here because everything it needs is here: the substituted
// signature, and the arena types already live in. What it does not get is a
// body index -- that is a table a unit owns and linking rebases, so it stays
// SYMBOL_FUNC_NO_BODY until codegen reserves a slot for it.
static void declare_generic_methods(TypeRegistry *registry, Type *type, const TypeDef *generic,
                                    const Type *const *args, size_t arg_count) {
    for (size_t i = 0; i < generic->method_count; i++) {
        const GenericMethod *method = &generic->methods[i];

        Symbol *symbol = arena_alloc(registry->arena, sizeof(Symbol));

        // The receiver is parameter zero, as it is for every method: what a
        // call writes follows it.
        const Type **params = arena_alloc(registry->arena, (method->param_count + 1) * sizeof(const Type *));

        params[0] = substitute(registry, method->receiver, args, arg_count);

        for (size_t p = 0; p < method->param_count; p++) {
            params[p + 1] = substitute(registry, method->params[p], args, arg_count);
        }

        *symbol = (Symbol){
            .kind = SYMBOL_FUNC,
            .func =
                {
                    .name = method->name,
                    .return_type = substitute(registry, method->result, args, arg_count),
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

const Type *type_registry_instantiate(TypeRegistry *registry, const TypeDef *def, const TypeArg *args,
                                      size_t arg_count) {
    assert(def && "an instantiation names a declaration");
    assert(arg_count == def->param_count && "an instantiation was given the wrong argument count");

    TypeApp app = {
        .ctor = TYPE_CTOR_NOMINAL,
        .def = def,
        .args = args,
        .arg_count = arg_count,
    };

    Type **existing = application_lookup(registry, app);
    if (existing) {
        return *existing;
    }

    // An instantiation mentions a parameter exactly when an argument brought
    // one: what it is built of is the declaration read under these arguments,
    // so nothing can appear in it that they did not supply.
    bool has_param = false;

    for (size_t i = 0; i < arg_count; i++) {
        has_param |= args[i].kind == TYPE_ARG_TYPE && type_has_param(args[i].type);
    }

    // A run of one argument, as many as another says. Laid out from the
    // arguments rather than from a field list, which is what the shape says.
    if (def->shape == TYPE_SHAPE_ARRAY) {
        assert(arg_count == 2 && args[0].kind == TYPE_ARG_TYPE && args[1].kind == TYPE_ARG_CONST &&
               "an array is an element and a length");

        Type *type = type_create(registry->arena, TYPE_ARRAY, def->name);

        type->array.element = args[0].type;
        type->array.length = args[1].value;
        type->has_param = has_param;
        type->decl = def;

        // Interned before the drop is derived, so a recursive element -- an
        // array of a struct holding one -- finds this entry rather than
        // building a second.
        application_insert(registry, app, type);

        type_registry_drop_of(registry, type);

        return type;
    }

    // A struct in every way that is laid out: the fields are where its width,
    // its alignment and what it owns all come from. What the declaration adds
    // is that they may have been written in terms of a parameter.
    Type *type = type_create(registry->arena, TYPE_STRUCT, def->name);

    // The declaration this instantiates, which is where its fields are read
    // from and where its shared methods hang.
    type->decl = def;
    type->has_param = has_param;

    // Interned before anything is substituted, so a field or a receiver naming
    // this same instantiation -- which every method's receiver does -- finds
    // this entry rather than building a second and recursing forever.
    application_insert(registry, app, type);

    // Applied to nothing, so substituting is the identity: this type's fields
    // are the declaration's, read through it rather than derived. What lets the
    // type exist before they do -- a struct's name is interned when it is
    // declared, and its fields arrive when they resolve.
    //
    // That is the whole of why a forward reference works. 'struct A { b: box B
    // }' interns B here to build the 'box B', and B's own fields land on the
    // declaration this points at, whenever they are settled.
    if (arg_count == 0) {
        return type;
    }

    const Type *type_args[GAB_MAX_TYPE_PARAMS];

    for (size_t i = 0; i < arg_count; i++) {
        assert(args[i].kind == TYPE_ARG_TYPE && "a record's parameters are types");

        type_args[i] = args[i].type;
    }

    TypeFields *substituted = arena_alloc(registry->arena, sizeof(TypeFields));
    TypeField *fields = arena_alloc(registry->arena, def->field_count * sizeof(TypeField));

    // Published before any of them is substituted, and counted as they are
    // filled: substituting a field may reach this same instantiation, and what
    // it finds must be a struct of the fields settled so far rather than a
    // count standing over memory nothing has written.
    *substituted = (TypeFields){.fields = fields, .count = 0};

    type->record.substituted = substituted;

    for (size_t i = 0; i < def->field_count; i++) {
        fields[i] = (TypeField){
            .name = def->fields[i].name,
            .type = substitute(registry, def->fields[i].type, type_args, arg_count),
        };

        substituted->count++;
    }

    type_registry_drop_of(registry, type);

    declare_generic_methods(registry, type, def, type_args, arg_count);

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
