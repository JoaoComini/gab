#ifndef GAB_TYPE_APP_H
#define GAB_TYPE_APP_H

// How a parameterized type is keyed for interning. The registry's business
// rather than the type's: a finished Type states what it is built of, while
// this is what a lookup carries to find whether that Type already exists.

#include "type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One argument of an application. A type or a compile-time value: '[T; N]'
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
// '[int; 3]' must find one Type: that is what this is looked up by.
//
// A key and only a key. What a type is built of lives in the type itself, so
// that an array's element is one fact rather than two that could disagree.
typedef struct TypeApp {
    TypeCtor ctor;

    // The declaration a nominal constructor came from -- what 'Vec' names. NULL
    // for the built-in constructors, which nothing declares and whose tag
    // already tells them apart.
    const TypeDef *def;

    const TypeArg *args;
    size_t arg_count;
} TypeApp;

size_t type_app_hash_of(TypeApp app);
bool type_app_equals(TypeApp app, TypeApp other);

#endif
