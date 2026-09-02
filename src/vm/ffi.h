#ifndef GAB_VM_FFI_H
#define GAB_VM_FFI_H

#include "arena.h"
#include "binding.h"
#include "type/type_registry.h"
#include "vm/args.h"

#include <stdbool.h>

typedef struct FfiSignature FfiSignature;

/* The call interface for 'function' calling 'symbol', or NULL with 'out_reason' set to why the
 * declaration cannot be expressed to C. */
FfiSignature *ffi_signature_prepare(Arena *arena, TypeRegistry *registry, const Function *function,
                                    void *symbol, const char **out_reason);

void ffi_invoke(const FfiSignature *signature, Args *args);

#endif
