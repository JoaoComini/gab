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

    // Declared on the bare 'Array' rather than on any '[T; N]': what a length
    // answers does not depend on the element, so one entry serves every
    // instantiation and each finds it through its declaration.
    builtin_register_shared_method(vm, type_registry_array_def(registry), "len", array_len,
                                   type_registry_get_primitive(registry, TYPE_INT), NULL, 0);
}
