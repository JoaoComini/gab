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

static const TypeLayout empty_layout = {.size = 0, .alignment = 1};

static Type *register_builtin(TypeRegistry *registry, TypeKind kind, String *name) {
    return type_create(registry->arena, kind, name);
}

static void register_primitives(TypeRegistry *registry, const TypePrimitiveNames *names) {
    registry->primitives.int_type = register_builtin(registry, TYPE_INT, names->int_name);
    registry->primitives.float_type = register_builtin(registry, TYPE_FLOAT, names->float_name);
    registry->primitives.bool_type = register_builtin(registry, TYPE_BOOL, names->bool_name);

    registry->primitives.byte_type = register_builtin(registry, TYPE_BYTE, names->byte_name);

    registry->primitives.str_type = register_builtin(registry, TYPE_STR, names->str_name);

    registry->primitives.error_type = register_builtin(registry, TYPE_ERROR, names->error_name);
}

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

    case TYPE_STR:
    case TYPE_ERROR:
        *size = 0;
        *alignment = 1;
        return true;

    default:
        return false;
    }
}

static void layout_of_array(TypeRegistry *registry, const Type *type, size_t *size, size_t *alignment) {
    const TypeLayout *element = type_registry_layout_of(registry, type_array_element(type));

    *size = element->size * (size_t)type_array_length(type);
    *alignment = element->alignment;
}

static void layout_of_indirect(const Type *type, size_t *size, size_t *alignment) {
    *size = sizeof(void *);
    *alignment = _Alignof(void *);

    if (type->kind == TYPE_BLOCK) {
        *size = align_up(*size + 2 * sizeof(int32_t), *alignment);
        return;
    }

    if (type_metadata_of(type_pointee(type)) == TYPE_META_LENGTH) {
        *size = align_up(*size + sizeof(int32_t), *alignment);
    }
}

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
    registry->generics = generic_key_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->signatures = signature_key_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->applications = type_intern_create_alloc(arena_allocator(arena), TYPE_REGISTRY_INITIAL_CAPACITY);
    registry->arena = arena;

    memset(registry->params, 0, sizeof(registry->params));

    register_primitives(registry, names);

    return registry;
}

void type_registry_destroy(TypeRegistry *registry) {
    type_intern_destroy(registry->applications);
    method_key_destroy(registry->methods);
    generic_key_destroy(registry->generics);
    signature_key_destroy(registry->signatures);
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

    const Type *type = decl->def->param_count == 0 ? type_registry_apply(registry, decl->def, NULL, 0) : NULL;

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

    TypeDef *def = arena_alloc(registry->arena, sizeof(TypeDef));

    *def = (TypeDef){.name = name, .fields = owned, .field_count = field_count};

    const TypeDecl decl = {.def = def};

    return type_registry_declare(registry, &decl);
}

static MethodKey method_key_of(const Type *type, const String *name) {
    const TypeDef *def = type_decl(type);

    return (MethodKey){.def = def, .type = def ? NULL : type, .name = name};
}

bool type_registry_add_method(TypeRegistry *registry, const Type *type, String *name, Symbol *method) {
    assert(type && name && method && "a method is a type, a name and a body");

    if (type_registry_find_method(registry, type, name)) {
        return false;
    }

    method_key_insert(registry->methods, method_key_of(type, name), method);
    return true;
}

static Symbol *substitute_signature(TypeRegistry *registry, const GenericMethod *method,
                                    const Type *const *args, size_t arg_count);

static MethodKey generic_key_of(const TypeDef *def, const String *name) {
    return (MethodKey){.def = def, .type = NULL, .name = name};
}

static const GenericMethod *find_generic(TypeRegistry *registry, const TypeDef *def, const String *name) {
    GenericMethod **stored = generic_key_lookup(registry->generics, generic_key_of(def, name));

    return stored ? *stored : NULL;
}

bool type_registry_declare_generic(TypeRegistry *registry, TypeDef *def, const GenericMethod *method) {
    assert(def && method && method->name && "a generic method is a declaration, a name and a signature");

    if (find_generic(registry, def, method->name)) {
        return false;
    }

    GenericMethod *owned = arena_alloc(registry->arena, sizeof(GenericMethod));
    *owned = *method;

    generic_key_insert(registry->generics, generic_key_of(def, method->name), owned);

    return true;
}

static const GenericMethod *declared_method(TypeRegistry *registry, const Type *type, const String *name) {
    const TypeDef *def = type_decl(type);

    if (!def) {
        return NULL;
    }

    return find_generic(registry, def, name);
}

Symbol *type_registry_find_method(TypeRegistry *registry, const Type *type, const String *name) {
    if (!type) {
        return NULL;
    }

    Symbol **stored = method_key_lookup(registry->methods, method_key_of(type, name));
    if (stored) {
        return *stored;
    }

    Symbol **cached = signature_key_lookup(registry->signatures, (SignatureKey){.type = type, .name = name});
    if (cached) {
        return *cached;
    }

    const GenericMethod *method = declared_method(registry, type, name);
    if (!method) {
        return NULL;
    }

    const Type *args[GAB_MAX_TYPE_PARAMS];

    for (size_t i = 0; i < type_arg_count(type); i++) {
        assert(type_args(type)[i].kind == TYPE_ARG_TYPE && "a method's parameters are types");

        args[i] = type_args(type)[i].type;
    }

    Symbol *symbol = substitute_signature(registry, method, args, type_arg_count(type));

    signature_key_insert(registry->signatures, (SignatureKey){.type = type, .name = name}, symbol);

    return symbol;
}

static Type *intern(TypeRegistry *registry, const Type *key) {
    Type **existing = type_intern_lookup(registry->applications, key);
    if (existing) {
        return *existing;
    }

    Type *type = arena_alloc(registry->arena, sizeof(Type));
    *type = *key;

    type_intern_insert(registry->applications, type, type);

    return type;
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
    Type key = type_init(TYPE_ARRAY, NULL);

    key.array.element = element;
    key.array.length = length;
    key.has_param = type_has_param(element);

    Type *type = intern(registry, &key);

    type_registry_drop_of(registry, type);

    return type;
}

static const Type *indirect_to(TypeRegistry *registry, TypeKind kind, const Type *inner) {
    Type key = type_init(kind, NULL);

    key.indirect.pointee = inner;
    key.has_param = type_has_param(inner);

    Type *type = intern(registry, &key);

    if (!type->has_param) {
        type_registry_drop_of(registry, type);
    }

    return type;
}

const Type *type_registry_box_to(TypeRegistry *registry, const Type *inner) {
    return indirect_to(registry, TYPE_BOX, inner);
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

static Symbol *substitute_signature(TypeRegistry *registry, const GenericMethod *method,
                                    const Type *const *args, size_t arg_count) {
    Symbol *symbol = arena_alloc(registry->arena, sizeof(Symbol));

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

    return symbol;
}

const Type *type_registry_instantiate(TypeRegistry *registry, const TypeDef *def, const TypeArg *args,
                                      size_t arg_count) {
    assert(def && "an instantiation names a declaration");
    assert(arg_count == def->param_count && "an instantiation was given the wrong argument count");

    Type key = type_init(TYPE_STRUCT, def->name);

    key.decl = def;
    key.args = args;
    key.arg_count = arg_count;

    for (size_t i = 0; i < arg_count; i++) {
        key.has_param |= args[i].kind == TYPE_ARG_TYPE && type_has_param(args[i].type);
    }

    Type **existing = type_intern_lookup(registry->applications, &key);
    if (existing) {
        return *existing;
    }

    TypeArg *owned = arena_alloc(registry->arena, arg_count * sizeof(TypeArg));
    memcpy(owned, args, arg_count * sizeof(TypeArg));

    key.args = owned;

    Type *type = intern(registry, &key);

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

    return type;
}

const Type *type_registry_block_of(TypeRegistry *registry, const Type *element) {
    return indirect_to(registry, TYPE_BLOCK, element);
}

const Type *type_registry_ref_to(TypeRegistry *registry, const Type *inner) {
    return indirect_to(registry, TYPE_REF, inner);
}

const Type *type_registry_ptr_to(TypeRegistry *registry, const Type *pointee) {
    return indirect_to(registry, TYPE_PTR, pointee);
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

        .byte_name = string_from_cstr(strings, "byte"),
        .str_name = string_from_cstr(strings, "str"),

        .error_name = string_from_cstr(strings, "<error>"),
    };
}
