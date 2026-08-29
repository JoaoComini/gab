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
    // The declaration an application instantiates: every '[T; N]' names the
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
    const TypeDef *decl;

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

        // TYPE_STRUCT: the fields the layout came from. A string's two are the
        // block holding its characters and how many of them are live.
        //
        // Not TYPE_STR: those characters are what a 'str' is, so it holds no
        // fields naming them. What does is a reference to one, and that is the
        // reference's own shape rather than anything read off the pointee.
        struct {
            // Set for an instantiation applied to no arguments, whose fields
            // are its declaration's unsubstituted -- so they are read from
            // 'decl' rather than held here, and the two cannot disagree.
            //
            // What lets the type be interned before its fields are known: a
            // struct's name is bound when it is declared, and a sibling's field
            // reaches this type while the declaration is still empty.
            bool through_def;

            // Only for an instantiation given arguments, whose fields are the
            // declaration's with those substituted in. A real computation
            // rather than a copy, which is why these are held.
            TypeField *fields;
            size_t field_count;
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
