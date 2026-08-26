#include "builtin/builtin.h"

#include "object.h"
#include "type_registry.h"
#include "vm/args.h"
#include "vm/vm.h"

// 'xs.len()'. Reads the count the receiver's header already carries, which is
// the same count every bounds check reads.
static void array_len(Args *args) { args_return_int(args, args_array(args, 0).length); }

void builtin_register_array(VM *vm) {
    TypeRegistry *registry = vm->env.global_scope.type_registry;

    // Declared on the bare 'Array', which no slot ever holds: what reaches this
    // set is each 'Array T' through its 'owner'. The receiver is that same bare
    // type, so the method is written once however many element types exist.
    Type *array_type = registry->builtins.array_type;

    builtin_register_method(vm, array_type, array_type, "len", array_len, registry->builtins.int_type, NULL,
                            0);
}
