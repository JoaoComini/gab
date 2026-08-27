#ifndef GAB_TYPE_H
#define GAB_TYPE_H

#include "allocator.h"
#include "arena.h"
#include "string/string.h"
#include "string/string_ref.h"
#include "util/hash_map.h"
#include "util/list.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_BOOL,

    // An address and nothing more: what a string's characters and an array's
    // elements are reached through. Carries what it points at in 'inner', so a
    // walk over a block knows its stride without asking the header naming it.
    //
    // Makes no claim beyond the address. It does not own what it names, does
    // not borrow it, and says nothing about how long that memory lives or
    // whether it is live at all -- which is what separates it from both
    // spellings of an indirection. Freeing a block is therefore always the
    // business of the header that knows how many of its elements are live.
    TYPE_PTR,

    // A header by value: the address of the characters and their count. Nominal
    // rather than structural, so it is interned once like the other builtins.
    TYPE_STRING,

    // A header owning a run of elements: where they are and how many. Laid out
    // like a string and freed like one, differing in that its element is
    // whatever it was written over rather than always a byte.
    TYPE_ARRAY,
    TYPE_STRUCT,

    // An indirection that owns what it names, and one that borrows it. Two
    // kinds rather than one carrying a flag, because whether a slot frees what
    // it names is the whole difference between them, and reading it off the
    // kind is what keeps every other question -- a deref, a field access, a
    // method receiver -- from having to ask about a flag it does not care
    // about. Both carry their pointee in 'inner'.
    //
    // How wide one is at run time follows from the pointee rather than being
    // stored here, so a pointee that one day needs a length beside its address
    // changes what the constructor computes and nothing else.
    TYPE_BOX,
    TYPE_REF,
    TYPE_UNKNOWN,
    TYPE_ERROR,
} TypeKind;

// The widest a laid-out type may be. A field's byte offset and an array's
// element offset both ride in an 8-bit instruction operand, so a type wider
// than this has members no instruction could reach.
//
// Stated here rather than read from the instruction encoding, because it is the
// resolver that must reject the type: by the time codegen is picking operands,
// the diagnostic naming the offending declaration is long gone.
#define GAB_MAX_TYPE_BYTES 255

typedef struct Type Type;

/*
    A type constructor, and what it was applied to.

    Every parameterized type in the language is one of these: 'box T', 'ref T',
    'ptr T' and 'Array T,N' differ in which constructor and how many arguments,
    not in kind. One interning table keyed by an application therefore serves
    all of them, and serves a generic 'List T' or 'Map K,V' with no new table --
    which is why this is a key rather than one map per constructor.
*/
typedef enum {
    // The built-in constructors, named directly by the compiler.
    TYPE_CTOR_BOX,
    TYPE_CTOR_REF,
    TYPE_CTOR_PTR,

    // A constructor with a declaration behind it: 'Array' today, and whatever
    // a generic declaration introduces later.
    TYPE_CTOR_NOMINAL,
} TypeCtor;

// One argument of an application. A type or a compile-time value: 'Array T,N'
// is one of each. Tagged rather than promoting a value into a Type, because a
// length has no size and no alignment to answer for, and every question asked
// of a Type would have to special-case one that is really a number.
typedef struct TypeArg {
    enum {
        TYPE_ARG_TYPE,
        TYPE_ARG_CONST,
    } kind;

    union {
        Type *type;
        int32_t value;
    };
} TypeArg;

// The interning key. Types compare by pointer identity, so two mentions of
// 'Array int,3' must find one Type: that is what this is looked up by.
typedef struct TypeApp {
    TypeCtor ctor;

    // The declaration a nominal constructor came from -- the bare 'Array'.
    // NULL for the built-in constructors, which their tag already tells apart.
    Type *decl;

    const TypeArg *args;
    size_t arg_count;
} TypeApp;

size_t type_app_hash_of(TypeApp app);
bool type_app_equals(TypeApp app, TypeApp other);

// Frees what one value of a type owns, leaving the value itself to its holder.
// Selected once, when the layout is computed, so that freeing never asks what
// kind a type is: a type that owns nothing has none of these at all, and the
// free path tests for that rather than calling one to learn there is nothing
// to do.
//
// Takes its type because a walk over fields needs the offsets, and recurses
// through the field's own function rather than flattening -- an owning shape
// whose bounds are not in the type, such as an array counted at run time, can
// then be its own function rather than a case in a shared walk.
typedef void (*DropFn)(Allocator allocator, const Type *type, void *value);

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

    // What an indirection names -- a TYPE_BOX, a TYPE_REF or a TYPE_PTR. NULL
    // for every other kind.
    Type *inner;

    // The methods declared with this type as their receiver. NULL until the
    // first one is, so a struct nobody declares a method on costs nothing.
    MethodMap *methods;

    // What freeing a value of this type must free, or NULL when it owns
    // nothing. Set by type_layout_compute.
    DropFn drop;

    // For a borrowing type that shares another's identity -- 'str' and
    // 'String' -- the owning one. NULL everywhere else. Method lookup follows
    // it so that one declaration serves both.
    Type *owner;

    // What this type was built by applying, for a type the registry interned.
    // Zeroed for a builtin and for a struct, which are named rather than
    // constructed.
    //
    // Kept so that the arguments stay readable off the type: an array's element
    // and its length are what its application was given, rather than something
    // recovered from the layout they produced.
    TypeApp app;
};

Type *type_create(Arena *arena, TypeKind kind, String *name);
Type *type_struct_create(Arena *arena, String *name, size_t max_fields);

void type_add_field(Type *type, String *name, Type *field_type);
void type_layout_compute(Type *type);

// Whether reaching the value means going through an indirection -- what a
// deref, an auto-deref, and a field access all ask. Says nothing about
// ownership: a 'ref T' is as indirect as a 'box T'.
bool type_is_indirect(const Type *type);

// What one element of an array is, and how many it holds. Both are what the
// application was given, so the element a walk strides by and the count it
// stops at are read from the same place the type was interned on.
Type *type_array_element(const Type *type);
int32_t type_array_length(const Type *type);

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

// A type as the source wrote it, before any name is looked up. The syntactic
// counterpart of Type: this is what a type position parses into, and the
// resolver evaluates it to the interned Type it names.
//
// A tree rather than a name plus a count of indirections, because the
// constructors nest freely and one of them takes arguments. Anything flatter
// needs a field per constructor that does not fit -- which is what an array's
// element was -- and a width to bound the nesting, which is a limit on what a
// program may say rather than on anything real.
typedef struct TypeExpr TypeExpr;

void type_expr_destroy(TypeExpr *expr);

#define type_expr_list_item_free(item) type_expr_destroy(item)
GAB_LIST(TypeExprList, type_expr_list, TypeExpr *)

typedef enum {
    // A name, possibly qualified: 'int', 'Player', 'Module::Type'. The leaf
    // every other kind bottoms out in.
    TYPE_EXPR_NAME,

    // 'box T' and 'ref T', each wrapping the one level it spells.
    TYPE_EXPR_BOX,
    TYPE_EXPR_REF,

    // A constructor applied to arguments: 'Array int' today, and whatever takes
    // more than one later. A list rather than a single argument, so that a
    // second one needs no second field.
    TYPE_EXPR_APPLY,
} TypeExprKind;

struct TypeExpr {
    TypeExprKind kind;

    // The name, for TYPE_EXPR_NAME. A qualified name is kept as one ref over
    // the source, so the resolver sees it exactly as the registry stores it.
    StringRef name;

    union {
        // What a 'box' or a 'ref' wraps.
        struct {
            TypeExpr *inner;
        } indirect;

        struct {
            TypeExpr *base;
            TypeExprList args;

            // How many elements, for 'Array T,N'. An integer literal rather
            // than an argument of its own: a length is not a type, and the one
            // constructor that takes one always takes exactly one.
            int32_t length;
        } apply;
    };
};

TypeExpr *type_expr_name(StringRef name);
TypeExpr *type_expr_indirect(TypeExprKind kind, TypeExpr *inner);
TypeExpr *type_expr_apply(TypeExpr *base);

#endif
