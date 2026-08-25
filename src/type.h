#ifndef GAB_TYPE_H
#define GAB_TYPE_H

#include "arena.h"
#include "string/string.h"
#include "string/string_ref.h"
#include "util/hash_map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_BOOL,

    // A header by value: the address of the characters and their count. Nominal
    // rather than structural, so it is interned once like the other builtins.
    TYPE_STRING,
    TYPE_STRUCT,

    // An indirection: 'box T' owns what it names, 'ref T' borrows it. One kind
    // for both, because every operation that reaches through one -- a deref, a
    // field access, a method receiver -- does the same thing either way. Only
    // 'is_ref' distinguishes them, and only ownership reads it.
    TYPE_INDIRECT,
    TYPE_UNKNOWN,
    TYPE_ERROR,
} TypeKind;

typedef struct Type Type;

// A method is an ordinary function Symbol — same prototype index, same call
// path — so the map holds one. Declared rather than included: Symbol's own
// header reaches back here, and a method is only ever a function.
typedef struct Symbol Symbol;

// The methods of one struct type, keyed by the interned method name. Held on
// the Type rather than in a Scope because a method has no free-standing name:
// 'Player.update' and 'Enemy.update' must coexist, and neither should be
// reachable by writing 'update'. Go, Rust, and C++ all key methods by their
// receiver type for the same reason.
#define method_map_hash(key) (size_t)key
#define method_map_key_equals(key, other) key == other
#define method_map_key_dup(key) key
#define method_map_entry_free(key, value)

GAB_HASH_MAP(MethodMap, method_map, String *, Symbol *)

typedef struct TypeField {
    String *name;
    Type *type;

    size_t offset;
} TypeField;

struct Type {
    TypeKind kind;

    // NULL for a type whose identity is structural: a pointer is '*' plus its
    // inner and nothing more, so its printable form is derived on demand
    // rather than stored. Non-NULL only for nominal types — builtins and
    // structs — where the name is the identity.
    String *name;

    size_t size;
    size_t alignment;

    TypeField *fields;
    size_t field_count;

    // What a TYPE_INDIRECT names; NULL for every other kind.
    Type *inner;

    // Whether this type borrows rather than owns what it names. A 'ref T' is a
    // distinct Type from 'box T', interned separately, so that freeing an
    // object can tell from a field's type alone whether it owns what the field
    // names — which is what keeps the whole ownership story type-driven.
    //
    // Meaningful on an indirection and on a string, the two kinds that name
    // memory someone must free. Always false on the rest.
    bool is_ref;

    // The methods declared with this type as their receiver. NULL until the
    // first one is, so a struct nobody declares a method on costs nothing.
    MethodMap *methods;

    // For a borrowing type that shares another's identity -- 'ref string' and
    // 'string' -- the owning one. NULL everywhere else. Method lookup follows
    // it so that one declaration serves both.
    Type *owner;
};

Type *type_create(Arena *arena, TypeKind kind, String *name);
Type *type_struct_create(Arena *arena, String *name, size_t max_fields);

void type_add_field(Type *type, String *name, Type *field_type);
void type_layout_compute(Type *type);

// Whether reaching the value means going through an indirection -- what a
// deref, an auto-deref, and a field access all ask. Says nothing about
// ownership: a 'ref T' is as indirect as a 'box T'.
bool type_is_indirect(const Type *type);

// Whether a value of this type owns memory that must be freed when it dies.
// True of a 'box T' and of an owning string, false of every 'ref', and true of
// a struct exactly when some field of it owns.
//
// Distinct from type_is_indirect because the two questions came apart once a
// string could own: a string owns without being an indirection, and a 'ref T'
// is an indirection that owns nothing.
bool type_is_owned(const Type *type);

// Whether a value of this type duplicates by copying its bytes, which is true
// exactly when nothing it holds transitively owns. See the definition.
bool type_is_copyable(const Type *type);

bool type_field_offset(const Type *type, const String *name, size_t *out_offset);
const TypeField *type_find_field(const Type *type, const String *name);

// Declares a method on this type, creating the map on first use. Returns false
// if the name is already taken, which is a duplicate declaration.
bool type_add_method(Arena *arena, Type *type, String *name, Symbol *method);

// The method this type declares under that name, or NULL. Does not look
// through an indirection: the caller strips that first, since 'box Player' and
// 'Player' share one method set.
Symbol *type_find_method(const Type *type, const String *name);

// The most indirection levels a type spec may carry, bounded by the bits in
// 'ref_levels' -- every level records its own kind, so the mask is the limit.
#define TYPE_SPEC_MAX_DEPTH 32

typedef struct {
    StringRef name;

    // 0 for T, 1 for 'box T', 2 for 'box box T'.
    unsigned int indirect_depth;

    // Which levels borrow rather than own, bit 0 being the level nearest the
    // name: 'ref box T' is depth 2 with bit 1 set. A flag per level rather than
    // one for the spec, since the two spellings nest in any order.
    uint32_t ref_levels;
} TypeSpec;

TypeSpec *type_spec_create(StringRef name, unsigned int indirect_depth, uint32_t ref_levels);
void type_spec_destroy(TypeSpec *spec);

#endif
