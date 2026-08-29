#include "builtin/builtin.h"

#include "object.h"
#include "type/type_registry.h"
#include "vm/args.h"
#include "vm/vm.h"

// 'xs.len()'. Never runs: the resolver folds the call to the length its type
// carries. Registered so that the name resolves and its arity is checked where
// every other method's is.
static void array_len(Args *args) { (void)args; }

void builtin_register_array(VM *vm) {
    TypeRegistry *registry = vm->env.global_scope.type_registry;

    // Declared on the bare 'Array', which no slot ever holds: what reaches this
    // set is each '[T; N]' through its 'owner'. The receiver is that same bare
    // type, so the method is written once however many element types exist.
    const TypeDef *array_def = type_registry_array_def(registry);

    builtin_register_def_method(vm, array_def, NULL, "len", array_len,
                                type_registry_get_primitive(registry, TYPE_INT), NULL, 0);
}
