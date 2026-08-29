#include "type_internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(int32_t) == 4, "gab int must be 4 bytes");
_Static_assert(sizeof(float) == 4, "gab float must be 4 bytes");

Type *type_create(Arena *arena, TypeKind kind, String *name) {
    Type *type = arena_alloc(arena, sizeof(Type));
    type->kind = kind;
    type->name = name;
    type->decl = NULL;
    type->generic = NULL;

    // A parameter is the one kind that carries it of itself. Every other type
    // is given it by whatever it is built from, as that part is attached.
    type->has_param = kind == TYPE_PARAM;

    // One arm zeroed is every arm zeroed: whichever the kind reads, it reads
    // nothing rather than whatever the arena held.
    memset(&type->record, 0, sizeof(type->record));

    return type;
}

Type *type_struct_create(Arena *arena, String *name, size_t max_fields) {
    Type *type = type_create(arena, TYPE_STRUCT, name);

    if (max_fields > 0) {
        type->record.fields = arena_alloc(arena, max_fields * sizeof(TypeField));
    }

    return type;
}

void type_add_field(Type *type, String *name, const Type *field_type) {
    assert(type->record.fields && "struct was created without room for fields");

    type->record.fields[type->record.field_count++] = (TypeField){
        .name = name,
        .type = field_type,
    };
}

const TypeField *type_find_field(const Type *type, const String *name) {
    for (size_t i = 0; i < type->record.field_count; i++) {
        const TypeField *field = &type->record.fields[i];

        if (field->name == name) {
            return field;
        }
    }

    return NULL;
}

// A raw pointer is deliberately not one of these. Every caller is a
// language-level path -- an auto-deref, a lend, a field access -- and a 'ptr T'
// is reachable from none of them: it names a block the header beside it
// describes, and reaching through it is that header's business.
TypeMetadata type_metadata_of(const Type *type) {
    if (!type) {
        return TYPE_META_NONE;
    }

    switch (type->kind) {
    // How far the characters run is not in their type, so a reference to them
    // carries that count beside the address.
    case TYPE_STR:
        return TYPE_META_LENGTH;

    // Named by a bare address, which is what all but the handful that say
    // otherwise are.
    default:
        return TYPE_META_NONE;
    }
}

bool type_is_str_ref(const Type *type) {
    return type && type->kind == TYPE_REF && type->indirect.pointee &&
           type->indirect.pointee->kind == TYPE_STR;
}

// Whether a value of this type can be held at all.
//
// Read off the kind rather than stored, because it is what the kind means: the
// characters of a string run however far they run, and no slot can reserve room
// for that. A type that named something unsized would be answering for its
// kind, not for itself.
bool type_is_sized(const Type *type) {
    if (!type) {
        return true;
    }

    switch (type->kind) {
    // The characters themselves, however many there are. Reached only through a
    // reference, which carries the count the type does not.
    case TYPE_STR:
        return false;

    // A stand-in for a type rather than a type, so nothing may hold one. What
    // makes a parameter reaching a slot a diagnostic rather than a width of
    // zero computed from a kind that has none.
    case TYPE_PARAM:
        return false;

    default:
        return true;
    }
}

const Type *type_pointee(const Type *type) {
    if (!type) {
        return NULL;
    }

    switch (type->kind) {
    case TYPE_BOX:
    case TYPE_REF:
    case TYPE_PTR:
    case TYPE_BLOCK:
        return type->indirect.pointee;

    default:
        return NULL;
    }
}

const TypeField *type_fields(const Type *type) {
    if (!type) {
        return NULL;
    }

    switch (type->kind) {
    case TYPE_STRUCT:
        return type->record.fields;

    default:
        return NULL;
    }
}

size_t type_field_count(const Type *type) {
    if (!type) {
        return 0;
    }

    switch (type->kind) {
    case TYPE_STRUCT:
        return type->record.field_count;

    default:
        return 0;
    }
}

bool type_is_indirect(const Type *type) { return type && (type->kind == TYPE_BOX || type->kind == TYPE_REF); }

bool type_owns_through_an_address(const Type *type) {
    return type && (type->kind == TYPE_BOX || type->kind == TYPE_BLOCK);
}

bool type_holds_its_memory_inline(const Type *type) {
    for (size_t i = 0; i < type_field_count(type); i++) {
        if (type_fields(type)[i].type && type_fields(type)[i].type->kind == TYPE_BLOCK) {
            return true;
        }
    }

    return false;
}

const Type *type_array_element(const Type *type) {
    assert(type && type->kind == TYPE_ARRAY && "only an array has an element");

    return type->array.element;
}

int32_t type_array_length(const Type *type) {
    assert(type && type->kind == TYPE_ARRAY && "only an array has a length");

    return type->array.length;
}

bool type_is_owned(const Type *type) {
    if (!type) {
        return false;
    }

    switch (type->kind) {
    // The two indirections differ in exactly this, which is why they are two
    // kinds: one frees what it names and one does not.
    case TYPE_BOX:
        return true;
    case TYPE_REF:
        return false;

    // The memory is the value's own, and freeing it is what holding one means.
    case TYPE_BLOCK:
        return true;

    // A raw address claims nothing about what it names, so nothing frees
    // through one. The header naming a block is what owns it.
    case TYPE_PTR:
        return false;

    // An array owns exactly what its elements do: it is a run of them and holds
    // nothing else, so the element answers for the whole run however long it is.
    case TYPE_ARRAY:
        return type_is_owned(type_array_element(type));

    // Characters nothing holds. A 'ref str' names them and owns nothing; the
    // type itself is never a value, so it is never a value that owns.
    case TYPE_STR:
        return false;

    default:
        break;
    }

    // A struct is not itself an owner: it owns through whichever fields do, and
    // is freed field by field rather than as one value.
    for (size_t i = 0; i < type->record.field_count; i++) {
        if (type_is_owned(type->record.fields[i].type)) {
            return true;
        }
    }

    return false;
}

// Whether a value of this type can be duplicated by copying its bytes. An
// owning value cannot: two slots holding it would both free it. Anything
// reaching one transitively inherits that, so a struct is copyable exactly
// when every field is.
//
// Derived from the type rather than declared on it, so a struct becomes
// non-copyable the moment it is given a field that owns, and no declaration
// can disagree with what the type holds.

bool type_is_copyable(const Type *type) {
    if (!type) {
        return true;
    }

    switch (type->kind) {
    // Copying an owning pointer would make a second owner of memory only one of
    // them may free. A borrow and a raw address each carried no ownership to
    // duplicate.
    case TYPE_BOX:
        return false;
    case TYPE_REF:
    case TYPE_PTR:
        return true;

    // Copying one would make a second value freeing the same block, which is
    // what an owning pointer is refused for.
    case TYPE_BLOCK:
        return false;

    // An array copies exactly when its elements do: copying one duplicates each
    // of them, so a run of a non-copyable element is as uncopyable as one is.
    case TYPE_ARRAY:
        return type_is_copyable(type_array_element(type));

    // Never held, so never copied. What names characters is a reference, and
    // copying one of those duplicates no ownership.
    case TYPE_STR:
        return true;

    default:
        break;
    }

    for (size_t i = 0; i < type->record.field_count; i++) {
        if (!type_is_copyable(type->record.fields[i].type)) {
            return false;
        }
    }

    return true;
}

TypeKind type_kind(const Type *type) { return type->kind; }

String *type_name_of(const Type *type) { return type->name; }

const Type *type_decl(const Type *type) { return type->decl; }

size_t type_param_index(const Type *type) {
    assert(type && type->kind == TYPE_PARAM && "only a parameter has an index");
    return type->param.index;
}

// Whether a parameter is reachable from here.
//
// Read off the type rather than walked: a struct may hold a pointer to its own
// type, so following the parts is a cycle with no base case. Settled instead as
// each constructor attaches what it was given.
bool type_has_param(const Type *type) { return type && type->has_param; }

const GenericDecl *type_generic(const Type *type) { return type->generic; }
