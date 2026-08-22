#include "builtin/builtin.h"

#include "object.h"
#include "type_registry.h"
#include "vm/args.h"
#include "vm/interp.h"
#include "vm/vm.h"

#include <stddef.h>

// 's.len()'. Reads the count the receiver's header already carries.
static void string_len(Args *args) { args_return_int(args, args_string(args, 0).length); }

// 's.at(i)'. The character at an index, as its numeric value.
//
// An index outside the string fails the run rather than answering: there is no
// value that could mean 'no character' without a caller mistaking it for one.
static void string_at(Args *args) {
    GabStringValue string = args_string(args, 0);
    int32_t index = args_int(args, 1);

    if (index < 0 || (size_t)index >= (size_t)string.length) {
        vm_fail(args->vm, VM_RUN_ERR_EXTERN, "string index is out of range");
        return;
    }

    args_return_int(args, (unsigned char)string.data[index]);
}

void builtin_register_string(VM *vm) {
    TypeRegistry *registry = vm->env.global_scope.type_registry;

    Type *string_type = registry->builtins.string_type;
    Type *int_type = registry->builtins.int_type;

    builtin_register_method(vm, string_type, "len", string_len, int_type, NULL, 0);

    Type *const at_params[] = {int_type};
    builtin_register_method(vm, string_type, "at", string_at, int_type, at_params, 1);
}
