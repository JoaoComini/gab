#ifndef GAB_BUILTIN_H
#define GAB_BUILTIN_H

#include "type.h"
#include "vm/link.h"
#include "vm/vm.h"

// The methods a builtin type answers. Registered rather than known to the
// compiler, so that adding one is an entry in a table here instead of a case in
// the resolver and another in codegen. It costs a call where an instruction
// would do; nothing yet makes that worth a second mechanism.
void builtin_register_all(VM *vm);

// Declares one method on a builtin type: a Symbol in the type's method map, and
// an entry in the table OP_CALL_EXTERN indexes. The body is a GabExternFn
// because that is what a C body is here, whether the host wrote it or the VM
// did.
//
// The receiver is parameter zero, by value.
void builtin_register_method(VM *vm, Type *receiver, const char *name, GabExternFn body, Type *return_type);

// Each builtin type's methods, called by builtin_register_all.
void builtin_register_string(VM *vm);

#endif
