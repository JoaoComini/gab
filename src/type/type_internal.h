#ifndef GAB_TYPE_INTERNAL_H
#define GAB_TYPE_INTERNAL_H

#include "type.h"

struct Type {
    TypeKind kind;

    String *name;

    const TypeDef *decl;

    const TypeArg *args;
    size_t arg_count;

    bool has_param;

    union {
        struct {
            const Type *pointee;
        } indirect;

        struct {
            const TypeFields *substituted;
        } record;

        struct {
            const Type *element;
            int32_t length;
        } array;

        struct {
            size_t index;
        } param;
    };
};

#endif
