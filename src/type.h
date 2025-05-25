#ifndef GAB_TYPE_H
#define GAB_TYPE_H

#include "string_ref.h"

typedef enum {
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_BOOL,
    TYPE_UNKNOWN, // for inference or errors
} TypeKind;

typedef struct {
    TypeKind kind;
    StringRef name;
} Type;

typedef struct {
    StringRef name;
} TypeSpec;

TypeSpec *type_spec_create(StringRef name);
void type_spec_free(TypeSpec *spec);

#endif
