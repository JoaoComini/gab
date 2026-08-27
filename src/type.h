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

    // One byte: the stride a walk over characters advances by, and the unit a
    // block of them is counted in. Its own kind rather than an int of another
    // width, so that a kind names exactly one type -- and unspellable, since no
    // scope is given the name.
    TYPE_BYTE,

    // An address and nothing more: what a string's characters and an array's
    // elements are reached through. Carries what it points at, so a
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

    // The characters themselves, however many there are. Unsized: no slot holds
    // one, because how far it runs is not in the type. Reached only through a
    // 'ref str', which carries that count beside the address.
    TYPE_STR,

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
    // about. Both carry what they name as their pointee.
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

/*
    Types compare by pointer identity, so every one a program can name came from
    the registry that interned it. Everything but the registry therefore holds a
    'const Type *': it names an interned type, cannot be used to build one, and
    cannot be written through -- a type is finished when the registry hands it
    over.

    The registry builds with 'Type *' and hands back the const form.
*/
typedef struct Type Type;

/*
    What a reference to a type must carry besides the address.

    A value whose bounds are its own -- an int, a struct, an owning header --
    needs nothing: its type says how wide it is, so an address is the whole
    reference. A run of characters or elements does not: how many there are
    lives with the value rather than with the type, so a reference to one
    carries a count beside the address and is two words instead of one.

    Carried by the pointee rather than by each reference, so that what a 'ref T'
    costs follows from what T is and cannot disagree with it. A slice would
    answer TYPE_META_LENGTH for the same reason characters do, and a value
    reached through a table of methods would answer with that table -- the kind
    of naming a reference carries is what grows here, not the fact that it
    carries one.
*/
typedef enum {
    // Nothing: the pointee's type says how wide it is.
    TYPE_META_NONE,

    // How many elements the pointee runs to, for a type whose length is not in
    // its type: 'str' today, and a slice when one exists.
    TYPE_META_LENGTH,
} TypeMetadata;

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
        const Type *type;
        int32_t value;
    };
} TypeArg;

// The interning key. Types compare by pointer identity, so two mentions of
// 'Array int,3' must find one Type: that is what this is looked up by.
//
// A key and only a key. What a type is built of lives in the type itself, so
// that an array's element is one fact rather than two that could disagree.
typedef struct TypeApp {
    TypeCtor ctor;

    // The declaration a nominal constructor came from -- the bare 'Array'.
    // NULL for the built-in constructors, which their tag already tells apart.
    const Type *decl;

    const TypeArg *args;
    size_t arg_count;
} TypeApp;

size_t type_app_hash_of(TypeApp app);
bool type_app_equals(TypeApp app, TypeApp other);

/*
    What freeing one value of a type has to do, with every offset it needs
    already in it.

    Built once, where the layout is computed, and read by the free path alone.
    Baking the offsets in is what keeps a laid-out Type out of the free path: a
    walk that read them back off the type would make layout something the VM
    consults on every free, rather than a fact the compiler settles and spends.

    The shape a plan takes is the shape of what owns, not the shape of the type:
    a struct of forty fields of which one owns is a plan of one step. A type
    that owns nothing has no plan at all, and the free path tests for that
    rather than walking a plan to learn there is nothing to do.
*/
typedef struct DropPlan DropPlan;

typedef enum {
    // Free what the address at this offset names, then the plan of the pointee.
    // What 'new box T' allocates and what every owning pointer field holds.
    DROP_BOX,

    // Free the characters a string header names. The count sits beside the
    // address, since a block has no header to ask how far it runs.
    DROP_STRING,

    // Walk a run of elements, freeing what each owns. Strides by a width the
    // plan carries rather than by one read back off a type.
    DROP_ARRAY,

    // Run each step at its own offset. What a struct is, and the only kind
    // whose offsets a plan has to carry.
    DROP_FIELDS,
} DropKind;

// One thing a plan does, at a fixed offset into the value. The offset is
// absolute within the value the plan describes, so a walk adds it and recurses
// rather than accumulating a base.
typedef struct DropStep {
    size_t offset;
    const DropPlan *plan;
} DropStep;

struct DropPlan {
    DropKind kind;

    // DROP_BOX: what the pointee owns, or NULL when it owns nothing and freeing
    // the block is the whole of it.
    // DROP_ARRAY: what one element owns, which is why the run is walked at all.
    // DROP_FIELDS: unused; the steps carry the plans.
    const DropPlan *inner;

    // DROP_ARRAY: how far apart the elements are, and how many there are.
    size_t stride;
    int32_t length;

    // DROP_FIELDS: what owns, and where. Only the fields that own appear.
    const DropStep *steps;
    size_t step_count;
};

// A method is an ordinary function Symbol — same prototype index, same call
// path — so the map holds one. Declared rather than included: Symbol's own
// header reaches back here, and a method is only ever a function.
typedef struct Symbol Symbol;

typedef struct TypeField {
    String *name;
    const Type *type;
} TypeField;

/*
    Where a value of a type sits in memory: how wide it is, what it must be
    aligned to, and where each of its fields begins.

    Beside the types rather than on them, for the reason a method set and a drop
    plan are: what a type is was settled when it was interned, while how wide it
    is follows from what its parts are laid out as. Keeping them apart is what
    lets a generic answer its layout per instantiation without the type itself
    being rebuilt -- and what lets a type be finished when the registry hands it
    over.

    Read through the registry, which is what owns one. See
    type_registry_layout_of.
*/
typedef struct TypeLayout {
    size_t size;
    size_t alignment;

    // Where each field begins, in the order the type declares them. Indexed by
    // the same i that indexes type_fields, so a walk over the two reads one
    // field's name and its offset from the same position.
    //
    // NULL for a kind with no fields, which is the right no-op for a walk that
    // has no fields to make.
    const size_t *offsets;
    size_t offset_count;
} TypeLayout;

struct Type {
    TypeKind kind;

    // NULL for a type whose identity is structural: a pointer is '*' plus its
    // inner and nothing more, so its printable form is derived on demand
    // rather than stored. Non-NULL only for nominal types — builtins and
    // structs — where the name is the identity.
    String *name;

    // The same layout question asked of a reference rather than of a value:
    // whether a slot may hold one at all, and what naming it takes besides the
    // address. Beside the width for that reason -- a type that has no width of
    // its own is exactly one whose reference carries what it lacks.
    //
    // The declaration an application instantiates: every 'Array T,N' names the
    // bare 'Array'. NULL for a type that is not an instantiation.
    //
    // Only the constructor, never the arguments -- those are the application
    // this type was interned on. What it buys today is the method set, which
    // every instantiation of a declaration shares because the one method there
    // is does not read its element. A method that did could not be shared, and
    // this becomes the key an instantiated set is built from rather than a link
    // followed to another type's.
    //
    // Distinct from the relation a borrowed view has to what it borrows: 'str'
    // is reached from 'String' by lending, which is a step down the chain a
    // receiver already walks, not a set held somewhere else.
    const Type *decl;

    /*
        What the kind gives it, and nothing another kind would give.

        A struct has no pointee to be wrong about and an indirection has no
        field list, rather than every reader having to know which of thirteen
        fields its kind licenses. The same shape TypeExpr and ASTExpr already
        use, for the same reason: a kind with a payload is a sum, and a struct
        of every payload at once holds combinations that mean nothing.
    */
    union {
        // TYPE_BOX, TYPE_REF, TYPE_PTR: what the indirection names.
        struct {
            const Type *pointee;
        } indirect;

        // TYPE_STRUCT, TYPE_STRING: the fields the layout came from. A string's
        // two are its characters and their count.
        //
        // Not TYPE_STR: those characters are what a 'str' is, so it holds no
        // fields naming them. What does is a reference to one, and that is the
        // reference's own shape rather than anything read off the pointee.
        struct {
            TypeField *fields;
            size_t field_count;
        } record;

        // TYPE_ARRAY: a run of one element, as many as the length says.
        struct {
            const Type *element;
            int32_t length;
        } array;
    };
};

// What an indirection names, or NULL for a kind that names nothing. The walks
// asking how many levels deep something is read it that way, so "not an
// indirection" is the answer that stops them rather than a mistake.
const Type *type_pointee(const Type *type);

// The fields a layout was computed from. Empty for a kind laid out some other
// way, so a walk over them is the right no-op there.
const TypeField *type_fields(const Type *type);
size_t type_field_count(const Type *type);

/*
    Building a type, which only the registry does.

    Every type a program can name is owned by the registry that interned it --
    that is what makes pointer identity a sound comparison, and what a
    const Type * asserts. These are declared here because type.c defines them, and
    reaching for one outside the registry is building a type nothing interned.
*/
Type *type_create(Arena *arena, TypeKind kind, String *name);
Type *type_struct_create(Arena *arena, String *name, size_t max_fields);

void type_add_field(Type *type, String *name, const Type *field_type);

// What a reference to this type carries besides the address.
TypeMetadata type_metadata_of(const Type *type);

// Whether this is a reference to characters -- the borrowed way of naming them,
// as an owning 'String' is the other. Its own predicate because the shape is
// two levels deep and asked in several places: what accepts a lend, what a
// receiver reconciles to, what a C body reads, and what '==' and '..' take.
bool type_is_str_ref(const Type *type);

// Whether a value of this type can be held at all: an unsized type names
// something no slot, field or parameter may contain, and is reached only
// through a reference.
bool type_is_sized(const Type *type);

// Whether reaching the value means going through an indirection -- what a
// deref, an auto-deref, and a field access all ask. Says nothing about
// ownership: a 'ref T' is as indirect as a 'box T'.
bool type_is_indirect(const Type *type);

// What one element of an array is, and how many it holds. Both are what the
// application was given, so the element a walk strides by and the count it
// stops at are read from the same place the type was interned on.
const Type *type_array_element(const Type *type);
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

// Which field a name denotes, or NULL when the type has no such field. Where it
// begins is a layout question, so it is asked of the registry rather than here.
const TypeField *type_find_field(const Type *type, const String *name);

// The fields of 'lender' that a reference to 'pointee' carries, written into
// 'out' in the order the reference holds them, and how many there are.
//
// By name rather than by position: what a reference carries is its own shape,
// and a lender declaring more than that -- a capacity beside a count -- lends
// the fields asked for wherever it happens to keep them.
size_t type_lent_fields(const Type *lender, const Type *pointee, const TypeField **out, size_t max);

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
