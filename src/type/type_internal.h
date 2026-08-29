#ifndef GAB_TYPE_INTERNAL_H
#define GAB_TYPE_INTERNAL_H

// The representation itself, for the type module alone.
//
// Everything outside holds a 'const Type *' and asks through the accessors in
// type.h: a type is finished when the registry hands it over, and a reader that
// could reach into one could disagree with what the registry settled. Kept out
// of type.h for that reason rather than for compile time.

#include "type.h"

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
    // The declaration an application instantiates: the 'Vec' behind every
    // 'Vec<T>'. NULL for a type nothing declares -- a primitive, or one of the
    // built-in constructors, which the language spells rather than naming.
    //
    // What it buys is the fields: an instantiation applied to none reads them
    // straight through this, and one applied to arguments reads them through
    // this substituted with 'args' below.
    //
    // Distinct from the relation a borrowed view has to what it borrows: 'str'
    // is reached from 'String' by lending, which is a step down the chain a
    // receiver already walks, not a set held somewhere else.
    //
    // With 'args' below, this is the whole of what a nominal type is: a
    // declaration applied to arguments, which is rustc's Adt(AdtDef,
    // GenericArgs). Its fields are that declaration's with those arguments
    // substituted in, and are held by the registry rather than here -- so a
    // name may be interned before its fields resolve, and no copy of them can
    // disagree with the declaration.
    const TypeDef *decl;

    // What that declaration was applied to: the '<int>' behind 'Vec<int>'. The
    // other half of Adt(AdtDef, GenericArgs), and load-bearing rather than
    // decorative -- a method hangs on the declaration and its signature is that
    // declaration's substituted with these, so every call needs them from the
    // type alone. Part of what the type is interned on, too: 'Vec<int>' and
    // 'Vec<bool>' are one declaration and differ only here.
    //
    // Empty for a type no declaration built and for a plain struct, which is a
    // declaration applied to nothing.
    const TypeArg *args;
    size_t arg_count;

    // Whether a parameter is reachable from here, settled as this type is
    // built rather than walked on demand. A walk cannot answer it: a struct
    // may hold a pointer to its own type, and following that is a cycle with
    // no base case. Every constructor sets it from what it was given, so a
    // parameter can only ever arrive through a part that already carries it.
    bool has_param;

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

        // TYPE_STRUCT: where its fields were derived, for an instantiation
        // given arguments. Owned by the registry that derived them.
        //
        // Not TYPE_STR: a string's characters are what a 'str' is, so it holds
        // no fields naming them. What does is a reference to one, and that is
        // the reference's own shape rather than anything read off the pointee.
        //
        // NULL for one applied to no arguments, whose fields are its
        // declaration's unsubstituted and are read through 'decl' -- so a
        // struct interned before its fields resolve needs nothing written here
        // once they do.
        struct {
            const TypeFields *substituted;
        } record;

        // TYPE_ARRAY: a run of one element, as many as the length says.
        struct {
            const Type *element;
            int32_t length;
        } array;

        // TYPE_PARAM: which of its declaration's parameters this is. An index
        // rather than an identity, so the parameter a declaration writes and
        // the argument an instantiation supplies meet by position -- which is
        // what lets one interned parameter serve every declaration.
        struct {
            size_t index;
        } param;
    };
};

#endif
