#include "compile.h"
#include "gab.h"
#include "vm/codegen.h"
#include "vm/interp.h"
#include "vm/vm.h"

#include "ast/resolve.h"
#include "binding.h"
#include "lexer.h"
#include "parser.h"
#include "string/string.h"
#include "type/type_registry.h"
#include "vm/opcode.h"

#include <assert.h>
#include <string.h>

static size_t loaded_protos(const VM *vm) { return vm->program.prototypes.size; }

static FuncPrototype *loaded_proto(const VM *vm, size_t index) { return vm->program.prototypes.data[index]; }

static void test_vm_execute() {
    VM *vm = vm_create();

    compile_and_run(vm, "module test;\n"
                        "func execute(): bool {\n"
                        "let a = 2;\n"
                        "let b = 3;\n"
                        "let c = a + b * 5;\n"
                        "let d = (c - a) * ((b + 4) / (a + 1));\n"
                        "let e = d + (c * (a - b) + (b / (a + 2)));\n"
                        "let f = e - ((d + c) * (b - a));\n"
                        "let g = ((f + e) * (d - c)) / ((a + b) - (e / (d + 1)));\n"
                        "let h = g + f - e * (d + c - (b * a));\n"
                        "let i = (h / g) + (f - (e * (d / (c + (b - a)))));\n"
                        "let result : int = ((i + h) * (g - f) + (e / d)) - ((c + b) * (a - 1));\n"
                        "let compare = result == 13120;\n"
                        "if compare { let a = 10; let b = 2; return a * b == 20; } else { return false; }\n"
                        "}");

    vm_free(vm);
}

static void test_two_vms_are_independent() {
    VM *first = vm_create();
    VM *second = vm_create();

    compile_and_run(first, "module test;\n"
                           "func run(): int { let value : int = 1; return value; }");
    compile_and_run(second, "module test;\n"
                            "func run(): int { let value : int = 2; return value; }");

    assert(&first->env.strings != &second->env.strings);

    const Type *first_int = type_registry_get_primitive(first->env.global_scope.type_registry, TYPE_INT);
    const Type *second_int = type_registry_get_primitive(second->env.global_scope.type_registry, TYPE_INT);

    assert(type_name_of(first_int) != type_name_of(second_int));

    vm_free(first);

    assert(strcmp(type_name_of(second_int)->data, "int") == 0);

    compile_and_run(second, "module test;\n"
                            "func again(): int { return 3; }");

    vm_free(second);
}

static void test_empty_function_body() {
    VM *vm = vm_create();

    compile_and_run(vm, "module test;\n"
                        "func main() { }");

    assert(loaded_protos(vm) == 1);

    FuncPrototype *proto = loaded_proto(vm, 0);
    assert(proto->chunk->instructions.size == 1);
    assert(VM_DECODE_OPCODE(instruction_list_get(&proto->chunk->instructions, 0)) == OP_RETURN);

    vm_free(vm);
}

static void test_struct_typed_local() {
    VM *vm = vm_create();

    compile_and_run(vm, "module test;\n"
                        "struct Vec3 { x: float, y: float, z: float }\n"
                        "func main() { let v = Vec3 { x: 0.0, y: 0.0, z: 0.0 }; }");

    assert(loaded_protos(vm) == 1);

    vm_free(vm);
}

static void test_top_level_runs_as_frame_zero() {
    VM *vm = vm_create();

    compile_and_run(vm, "module test;\n"
                        "func main() { let a = 2; }");
    assert(vm->frame_count == 0);

    compile_and_run(vm, "module test;\n"
                        "let x: int = 7;");
    assert(vm->frame_count == 0);

    vm_free(vm);
}

static void test_types_survive_a_later_compile() {
    VM *vm = vm_create();

    compile_and_run(vm, "module test;\n"
                        "struct Player { health: int, mana: int }\n");

    const Type *player =
        scope_type_lookup(environment_module_scope(&vm->env, string_from_cstr(&vm->env.strings, "test")),
                          string_from_cstr(&vm->env.strings, "Player"));
    assert(player);

    size_t size = type_registry_size_of(vm->env.global_scope.type_registry, player);
    size_t field_count = type_registry_fields_of(vm->env.global_scope.type_registry, player)->count;

    compile_and_run(vm,
                    "module test;\n"
                    "func a(x: int, y: int): int { let q: int = x + y; let w: int = q * q; return w; }\n"
                    "func b(x: int, y: int): int { let q: int = x - y; let w: int = q * q; return w; }\n");

    assert(strcmp(type_name_of(player)->data, "Player") == 0);
    assert(type_registry_size_of(vm->env.global_scope.type_registry, player) == size);
    assert(type_registry_fields_of(vm->env.global_scope.type_registry, player)->count == field_count);

    vm_free(vm);
}

static void test_function_signatures_survive_a_later_compile() {
    VM *vm = vm_create();

    compile_and_run(vm, "module test;\n"
                        "struct Player { health: int, mana: int }\n"
                        "func on_update(p: Player, dt: float): int { return p.health; }\n");

    Binding *on_update =
        scope_binding_lookup(environment_module_scope(&vm->env, string_from_cstr(&vm->env.strings, "test")),
                             string_from_cstr(&vm->env.strings, "on_update"));
    assert(on_update && on_update->kind == BINDING_FUNC);
    assert(on_update->func->param_count == 2);

    compile_and_run(vm,
                    "module test;\n"
                    "func a(x: int, y: int): int { let q: int = x + y; let w: int = q * q; return w; }\n"
                    "func b(x: int, y: int): int { let q: int = x - y; let w: int = q * q; return w; }\n");

    assert(on_update->func->param_count == 2);
    assert(strcmp(type_name_of(on_update->func->params[0])->data, "Player") == 0);
    assert(strcmp(type_name_of(on_update->func->params[1])->data, "float") == 0);
    assert(strcmp(type_name_of(on_update->func->return_type)->data, "int") == 0);

    vm_free(vm);
}

static void test_prototypes_survive_a_later_compile() {
    VM *vm = vm_create();

    compile_and_run(vm, "module test;\n"
                        "func seven(): int { return 7; }\n");

    assert(loaded_protos(vm) == 1);

    const FuncPrototype *seven = loaded_proto(vm, 0);
    const Chunk *chunk = seven->chunk;
    int arity = seven->arity;

    compile_and_run(vm, "module test;\n"
                        "func a(x: int): int { return x; }\n"
                        "func b(x: int): int { return x; }\n"
                        "func c(x: int): int { return x; }\n"
                        "func d(x: int): int { return x; }\n"
                        "func e(x: int): int { return x; }\n"
                        "func f(x: int): int { return x; }\n"
                        "func g(x: int): int { return x; }\n"
                        "func h(x: int): int { return x; }\n");

    assert(loaded_protos(vm) > 1);
    assert(loaded_proto(vm, 0) == seven);
    assert(seven->chunk == chunk);
    assert(seven->arity == arity);

    vm_free(vm);
}

static void test_a_call_reaches_a_function_from_an_earlier_unit() {
    VM *vm = vm_create();

    compile_and_run(vm, "module M;\nfunc seven(): int { return 7; }\n");

    compile_and_run(vm, "module M;\n"
                        "func a(): int { return 1; }\n"
                        "func b(): int { return 2; }\n"
                        "func calls_across(): int { return seven(); }\n"
                        "let r: int = calls_across();\n");

    int32_t returned;
    memcpy(&returned, vm_slot_at(vm, 0), sizeof(returned));

    assert(returned == 7);

    vm_free(vm);
}

static void test_a_unit_that_fails_to_link_installs_nothing() {
    VM *vm = vm_create();

    compile_and_run(vm, "module test;\n"
                        "func first(): int { return 1; }\n");

    size_t protos = loaded_protos(vm);
    size_t types = vm->program.heap_shapes.size;

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->env.compile_arena, "<test>");

    FuncPrototype top_level;
    assert(!compile_unit(vm,
                         "module test;\n"
                         "struct Only { a: int }\n"
                         "func second(): int { let p: box Only = new Only; return p.a; }\n"
                         "extern func absent(x: int): int;\n",
                         &top_level, &diagnostics));

    assert(diagnostics_has_errors(&diagnostics));
    diagnostics_free(&diagnostics);

    assert(loaded_protos(vm) == protos);
    assert(vm->program.heap_shapes.size == types);

    vm_free(vm);
}

static void test_checking_a_unit_installs_nothing() {
    VM *vm = vm_create();

    compile_and_run(vm, "module test;\n"
                        "func first(): int { return 1; }\n");

    size_t protos = loaded_protos(vm);
    size_t types = vm->program.heap_shapes.size;

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->env.compile_arena, "<test>");

    Lexer lexer = lexer_create("module dry;\n"
                               "struct Only { a: int }\n"
                               "func second(): int { let p: box Only = new Only; return p.a; }\n",
                               vm->env.compile_arena, &vm->env.strings, &diagnostics);
    Parser parser = parser_create(&lexer, &diagnostics);
    ASTUnit *ast = ast_unit_create();

    assert(parser_parse(&parser, ast));

    Scope staging;
    scope_init_staging(&staging, vm->env.arena, &vm->env.strings,
                       environment_module_scope(&vm->env, string_from_cstr(&vm->env.strings, "dry")));

    assert(resolve_unit(vm->env.compile_arena, ast, &staging, vm->env.module_scopes, &diagnostics));

    Unit *unit = codegen_generate(ast, vm->env.arena, &vm->env.strings, staging.type_registry, &diagnostics);
    assert(unit);

    assert(link_check(&vm->program, unit, &diagnostics));

    assert(loaded_protos(vm) == protos);
    assert(vm->program.heap_shapes.size == types);
    assert(
        !scope_binding_lookup(environment_module_scope(&vm->env, string_from_cstr(&vm->env.strings, "dry")),
                              string_from_cstr(&vm->env.strings, "second")));

    unit_free(unit);
    ast_unit_destroy(ast);
    diagnostics_free(&diagnostics);

    vm_free(vm);
}

static void test_compile_once_run_many() {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->env.compile_arena, "<test>");

    FuncPrototype top_level;
    bool ok = compile_unit(vm,
                           "module test;\n"
                           "func seven(): int { return 7; }\nlet r: int = seven();\n",
                           &top_level, &diagnostics);
    assert(ok);

    diagnostics_free(&diagnostics);

    for (int i = 0; i < 3; i++) {
        interp_run_top_level(vm, &top_level);

        assert(vm->frame_count == 0);
        int32_t returned;
        memcpy(&returned, vm_slot_at(vm, 0), sizeof(returned));

        assert(returned == 7);
    }

    func_proto_free(&top_level);

    vm_free(vm);
}

static void test_compile_failure_is_reportable() {
    VM *vm = vm_create();

    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->env.compile_arena, "<test>");

    FuncPrototype top_level;
    bool ok = compile_unit(vm,
                           "module test;\n"
                           "func broken(: int { return",
                           &top_level, &diagnostics);

    assert(!ok);
    assert(diagnostics_has_errors(&diagnostics));

    diagnostics_free(&diagnostics);
    vm_free(vm);
}

static bool compile_ok(VM *vm, const char *source) {
    Diagnostics diagnostics;
    diagnostics_init(&diagnostics, vm->env.compile_arena, "<test>");

    FuncPrototype top_level;
    bool ok = compile_unit(vm, source, &top_level, &diagnostics);

    diagnostics_free(&diagnostics);

    if (ok) {
        func_proto_free(&top_level);
    }

    return ok;
}

static void test_modules_isolate_declarations() {
    VM *vm = vm_create();

    assert(compile_ok(vm, "module Player;\nfunc on_update(): int { return 1; }\n"));
    assert(compile_ok(vm, "module Enemy;\nfunc on_update(): int { return 2; }\n"));

    vm_free(vm);
}

static void test_modules_accumulate_across_units() {
    VM *vm = vm_create();

    assert(compile_ok(vm, "module Player;\nfunc helper(): int { return 7; }\n"));
    assert(compile_ok(vm, "module Player;\nfunc uses(): int { return helper(); }\n"));

    assert(!compile_ok(vm, "module Enemy;\nfunc bad(): int { return helper(); }\n"));

    vm_free(vm);
}

static void test_the_root_holds_only_builtins() {
    VM *vm = vm_create();

    assert(compile_ok(vm, "module M;\nfunc uses_builtins(a: int, b: float): int { return a; }\n"));

    assert(compile_ok(vm, "module N;\nfunc also_builtins(a: int): int { return a; }\n"));
    assert(!compile_ok(vm, "module P;\nfunc g(): int { return uses_builtins(1, 2.0); }\n"));

    vm_free(vm);
}

static void test_module_scope_does_not_change_pointer_lifetimes() {
    VM *vm = vm_create();

    const char *takes_address = "func f(): int {\n"
                                "  let x: int = 1;\n"
                                "  let p: ref int = x;\n"
                                "  return *p;\n"
                                "}\n";

    const char *escapes = "func g(): int {\n"
                          "  let outer: box int;\n"
                          "  { let inner: int = 1; outer = inner; }\n"
                          "  return 0;\n"
                          "}\n";

    char buffer[512];

    snprintf(buffer, sizeof(buffer), "module A;\n%s", takes_address);
    assert(compile_ok(vm, buffer));

    snprintf(buffer, sizeof(buffer), "module B;\n%s", escapes);
    assert(!compile_ok(vm, buffer));

    vm_free(vm);
}

static void test_a_load_name_replaces_nothing() {
    GabVM *handle = gab_vm_new();
    GabError err;

    assert(gab_load(handle, "same.gab", "module test;\nfunc first(): int { return 1; }\n", &err));
    assert(gab_load(handle, "same.gab", "module test;\nfunc second(): int { return 2; }\n", &err));

    assert(((VM *)handle)->program.top_levels.size == 2);

    gab_vm_free(handle);
}

int main() {
    test_modules_isolate_declarations();
    test_modules_accumulate_across_units();
    test_the_root_holds_only_builtins();
    test_module_scope_does_not_change_pointer_lifetimes();

    test_vm_execute();
    test_top_level_runs_as_frame_zero();
    test_two_vms_are_independent();
    test_empty_function_body();
    test_struct_typed_local();
    test_types_survive_a_later_compile();
    test_function_signatures_survive_a_later_compile();
    test_prototypes_survive_a_later_compile();
    test_a_call_reaches_a_function_from_an_earlier_unit();
    test_a_unit_that_fails_to_link_installs_nothing();
    test_checking_a_unit_installs_nothing();
    test_compile_once_run_many();
    test_compile_failure_is_reportable();
    test_a_load_name_replaces_nothing();

    return 0;
}
