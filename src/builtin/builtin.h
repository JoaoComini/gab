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
void builtin_register_method(VM *vm, const Type *declared_on, const Type *receiver, const char *name,
                             GabExternFn body, const Type *return_type, const Type *const *params,
                             size_t param_count);

// As builtin_register_method, for a function reached on the type rather than on
// a value: 'Type::name(args)'. Every parameter is in 'params', since nothing is
// the receiver.
void builtin_register_static(VM *vm, const Type *declared_on, const char *name, GabExternFn body,
                             const Type *return_type, const Type *const *params, size_t param_count);

// The types the standard library provides, registered with the registry before
// any scope is built from it: a scope names what is registered when it is
// created, so a type declared later would be invisible to it.
//
// Separate from the methods below because those need a VM -- an extern table to
// be numbered in -- while a type needs only the registry. That split is also
// what a compile with no VM sees: the types exist to be named, and nothing is
// callable on them.
void builtin_register_types(TypeRegistry *registry);

// Each builtin type's methods, called by builtin_register_all.
void builtin_register_string(VM *vm);

// The 'String' type itself, and the view it lends. Registered before any scope.
void builtin_register_string_type(TypeRegistry *registry);

// The methods every array answers, declared once on the bare 'Array' type that
// each '[T; N]' reaches through 'owner'. None of them depend on the element,
// so one set serves every array.
void builtin_register_array(VM *vm);

// The methods every 'Vec<T>' answers. Unlike an array's, these are declared on
// the 'Vec' declaration in terms of its parameter and turned into Symbols where
// each instantiation is interned -- 'push' takes a T, so there is no one set
// that could serve every element.
//
// This registers the installer the registry calls to do that, so a 'Vec<T>'
// named by any later compile gets its methods without the VM being asked again.
void builtin_register_vec(VM *vm);

// The 'Vec' declaration every 'Vec<T>' is instantiated from.
void builtin_register_vec_type(TypeRegistry *registry);

#endif
