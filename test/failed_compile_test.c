#include "gab.h"
#include "vm/vm.h"

#include <assert.h>

static void test_a_rejected_compile_costs_no_lasting_memory() {
    GabVM *vm = gab_vm_new();
    VM *inner = (VM *)vm;
    GabError err;

    size_t committed = inner->env.staging_arenas.size;

    const char *rejected = "module bad;\nfunc f(): int { return \"not an int\"; }\n";

    for (int i = 0; i < 50; i++) {
        assert(!gab_vm_load(vm, "bad", rejected, &err));
    }

    assert(inner->env.staging_arenas.size == committed);

    gab_vm_free(vm);
}

static void test_a_committed_compile_keeps_its_arena() {
    GabVM *vm = gab_vm_new();
    VM *inner = (VM *)vm;
    GabError err;

    size_t committed = inner->env.staging_arenas.size;

    assert(gab_vm_load(vm, "good", "module good;\nfunc f(): int { return 1; }\n", &err));

    assert(inner->env.staging_arenas.size == committed + 1);

    gab_vm_free(vm);
}

int main() {
    test_a_rejected_compile_costs_no_lasting_memory();
    test_a_committed_compile_keeps_its_arena();

    return 0;
}
