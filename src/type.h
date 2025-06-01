#ifndef GAB_TYPE_H
#define GAB_TYPE_H

#include "arena.h"
#include "string/string.h"
#include "string/string_ref.h"
#include "util/list.h"

typedef struct {
    StringRef name;
} TypeSpec;

TypeSpec *type_spec_create(StringRef name);
void type_spec_destroy(TypeSpec *spec);

typedef enum {
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_BOOL,
    TYPE_UNKNOWN, // for inference or errors
} TypeKind;

typedef struct {
    TypeKind kind;
    String *name;
} Type;

Type *type_create(Arena *arena, TypeKind kind, String *name);
void type_destroy(Type *type);

#define type_list_item_free
GAB_LIST(TypeList, type_list, Type *);

typedef struct {
    TypeList params;
    Type *return_type;
} FuncSignature;

FuncSignature *func_signature_create(Arena *arena, TypeList params, Type *return_type);

#endif
