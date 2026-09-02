#ifndef GAB_ARGS_H
#define GAB_ARGS_H

#include "object.h"
#include "type/type.h"
#include "type/type_registry.h"
#include "vm/vm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint8_t *args_address(Args *args, int index, const Type **out_type);

uint8_t *args_return_address(Args *args);

const Type *args_return_type(Args *args);

unsigned int args_type_slots(TypeRegistry *registry, const Type *type);

int32_t args_int(Args *args, int index);
float args_float(Args *args, int index);
bool args_bool(Args *args, int index);
StrRef args_string(Args *args, int index);

StringValue args_string_at(Args *args, int index);

ArrayValue args_array(Args *args, int index);
void *args_pointer(Args *args, int index);

void args_struct(Args *args, int index, void *out, size_t size);

void args_drop(Args *args, int index);

/* A box of the declared return type's pointee, or NULL with the call failed. */
void *args_box_return(Args *args);

void args_return_int(Args *args, int32_t value);
void args_return_float(Args *args, float value);
void args_return_bool(Args *args, bool value);
void args_return_pointer(Args *args, void *pointer);
void args_return_struct(Args *args, const void *data, size_t size);

bool args_string_copy(Args *args, const char *data, int32_t length, StringValue *out);

#endif
