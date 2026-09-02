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

unsigned int args_type_slots(TypeRegistry *registry, const Type *type);

/* A box of the declared return type's pointee, or NULL with the call failed. */
void *args_box_return(Args *args);

bool args_string_copy(Args *args, const char *data, int32_t length, StringValue *out);

#endif
