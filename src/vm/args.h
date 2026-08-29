#ifndef GAB_ARGS_H
#define GAB_ARGS_H

#include "object.h"
#include "type/type.h"
#include "type/type_registry.h"
#include "vm/vm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One call's view of the frame it was called with, for a body written in C
// rather than in bytecode. A builtin method and a host extern are both that,
// and both address their arguments through here.
//
// Reading a parameter that a body's own declaration does not have -- a bad
// index, or a type the declaration does not give it -- is a programming error
// and asserts. It is not a condition a caller can be handed and asked to
// recover from: the VM registered the signature a builtin reads against, and a
// host declared in script the one its C body implements. Neither has anywhere
// to go once the two disagree, and returning a zero would let the misread
// travel as data.

// Where a parameter's slots begin, counting from the frame's slot 0 -- which
// holds the return value, exactly as it does for a script callee. A multi-slot
// parameter occupies consecutive slots, so each index is found by walking the
// widths ahead of it rather than by indexing a table.
uint8_t *args_address(Args *args, int index, const Type **out_type);

// The frame's slot 0, which is where a callee leaves its result.
uint8_t *args_return_address(Args *args);

// Slots a value of this type occupies, matching codegen's tiling exactly: the
// two must agree or an argument lands in the wrong register.
unsigned int args_type_slots(TypeRegistry *registry, const Type *type);

// --- Reading arguments -----------------------------------------------------

int32_t args_int(Args *args, int index);
float args_float(Args *args, int index);
bool args_bool(Args *args, int index);
GabStrRef args_string(Args *args, int index);

// As args_string, for a parameter declared as a pointer to one. The two are
// separate because the declarations are: a 'String' or a 'str' parameter holds
// the header in its own slots, and a 'ref String' holds an address.
GabStringValue args_string_at(Args *args, int index);

GabArrayValue args_array(Args *args, int index);
void *args_pointer(Args *args, int index);

// 'size' is what the caller expects the parameter to occupy, which must be what
// its declared type does.
void args_struct(Args *args, int index, void *out, size_t size);

// --- Writing the result ----------------------------------------------------

void args_return_int(Args *args, int32_t value);
void args_return_float(Args *args, float value);
void args_return_bool(Args *args, bool value);
void args_return_pointer(Args *args, void *pointer);
void args_return_struct(Args *args, const void *data, size_t size);

// Writes a string result whose characters the caller's slot then owns. The
// bytes are copied into a heap object of their own, so a body may return
// characters it borrowed -- which is what makes 'clone' possible at all.
// Fails the run and returns false if the allocation does not succeed.
bool args_return_string_copy(Args *args, const char *data, int32_t length);

#endif
