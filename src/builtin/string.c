#include "builtin/builtin.h"

#include "type_registry.h"
#include "vm/args.h"
#include "vm/vm.h"

// 's.len()'. Reads the count the receiver's header already carries.
static void string_len(Args *args) { args_return_int(args, args_string(args, 0).length); }

void builtin_register_string(VM *vm) {
    TypeRegistry *registry = vm->env.global_scope.type_registry;

    Type *string_type = registry->builtins.string_type;

    builtin_register_method(vm, string_type, "len", string_len, registry->builtins.int_type);
}
