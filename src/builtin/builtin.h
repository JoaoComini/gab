#ifndef GAB_BUILTIN_H
#define GAB_BUILTIN_H

#include "type/type.h"
#include "vm/link.h"
#include "vm/vm.h"

#include <stddef.h>

// The methods a builtin type answers. Registered rather than known to the
// compiler, so that adding one is an entry in a table here instead of a case in
// the resolver and another in codegen. It costs a call where an instruction
// would do; nothing yet makes that worth a second mechanism.
void builtin_register_all(VM *vm);

// What a library says one of its types is: the name, the fields, and how many
// parameters those fields are written over. Zero for a plain struct.
//
// The declaration itself is built in the VM's arena rather than by the caller,
// because the type interned from it reads its fields for as long as it lives --
// a TypeDef on a stack frame is a use-after-free the type system cannot see.
typedef struct BuiltinTypeSpec {
    const char *name;
    size_t param_count;

    const TypeFieldSpec *fields;
    size_t field_count;

    // What a value of this type stands for, and which of its bytes name that
    // view. Both or neither, as in TypeDecl.
    const Type *derefs_to;
    const LentPart *lent_parts;
    size_t lent_part_count;
} BuiltinTypeSpec;

// Declares a type the standard library provides: interned on the VM's registry
// and named in its global scope, which is where every other type name lives.
//
// One call rather than two phases. The VM owns both the registry that interns
// and the scope that names, so a library says what a type is once and gets it
// back -- and may then declare methods on it with the calls below.
//
// Absent from a compile that never had a VM, which is what keeps 'String' out
// of the language and in the library that provides it.
//
// Returns the declaration rather than the type, since one taking parameters
// stands for no type until a mention supplies them. A caller wanting the type a
// declaration taking none stands for asks the registry for it, which is that
// declaration applied to nothing.
const TypeDef *builtin_declare(VM *vm, const BuiltinTypeSpec *spec);

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

// As builtin_register_method, for a set every instantiation of a declaration
// answers rather than one type's: an array's 'len' is the same whatever the
// element, so it is declared once on 'Array' and every '[T; N]' finds it.
//
// No receiver, because there is no one type to be parameter zero: a call takes
// the instantiation whose set answered.
void builtin_register_shared_method(VM *vm, const TypeDef *declared_on, const char *name, GabExternFn body,
                                    const Type *return_type, const Type *const *params, size_t param_count);

// As builtin_register_method, for a function reached on the type rather than on
// a value: 'Type::name(args)'. Every parameter is in 'params', since nothing is
// the receiver.
void builtin_register_static(VM *vm, const Type *declared_on, const char *name, GabExternFn body,
                             const Type *return_type, const Type *const *params, size_t param_count);

// Each builtin type's methods, called by builtin_register_all.
void builtin_register_string(VM *vm);

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

#endif
