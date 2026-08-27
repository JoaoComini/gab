#ifndef GAB_BUILTIN_H
#define GAB_BUILTIN_H

#include "type.h"
#include "vm/link.h"
#include "vm/vm.h"

#include <stddef.h>

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
// 'params' are those a call writes, so the receiver is not among them: it is
// parameter zero and is supplied here, which is what keeps a method from
// declaring a receiver other than the type it hangs on. A method taking only
// its receiver passes NULL and zero.
// 'declared_on' is the type whose method set the name lands in, and 'receiver'
// is what parameter zero takes. The two differ for a string: the set is the
// owning type's, since a borrow reads it through 'owner' and an owning string
// has no route the other way, while the receiver stays the borrow so that
// reading a string never asks for ownership of it.
void builtin_register_method(VM *vm, TypeHandle declared_on, TypeHandle receiver, const char *name,
                             GabExternFn body, TypeHandle return_type, TypeHandle const *params,
                             size_t param_count);

// Each builtin type's methods, called by builtin_register_all.
void builtin_register_string(VM *vm);

// The methods every array answers, declared once on the bare 'Array' type that
// each 'Array T' reaches through 'owner'. None of them depend on the element,
// so one set serves every array.
void builtin_register_array(VM *vm);

#endif
