#include "builtin/builtin.h"

#include "arena.h"
#include "string/string.h"
#include "symbol_table.h"
#include "type.h"
#include "type_registry.h"
#include "vm/link.h"
#include "vm/vm.h"

#include <stddef.h>

void builtin_register_method(VM *vm, Type *receiver, const char *name, GabExternFn body, Type *return_type) {
    Arena *arena = vm->env.arena;

    Symbol *symbol = arena_alloc(arena, sizeof(Symbol));
    symbol->kind = SYMBOL_FUNC;
    symbol->func.return_type = return_type;
    symbol->func.param_count = 1;
    symbol->func.params = arena_alloc(arena, sizeof(Type *));
    symbol->func.is_extern = true;
    symbol->func.name = string_from_cstr(&vm->env.strings, name);
    symbol->func.module = NULL;

    // The receiver is parameter zero, by value: a string is a header that
    // copies, and a method that only reads it wants no indirection.
    symbol->func.params[0] = receiver;

    symbol->func.func_index = vm->program.extern_protos.size;

    extern_proto_list_add(&vm->program.extern_protos, (ExternProto){.body = body, .symbol = symbol});

    type_add_method(arena, receiver, string_from_cstr(&vm->env.strings, name), symbol);
}

// These land at the bottom of the extern table, before any unit loads. That
// costs a unit's own externs nothing: the two tables are numbered apart, so a
// script function's index is unaffected by how many builtins exist.
void builtin_register_all(VM *vm) { builtin_register_string(vm); }
