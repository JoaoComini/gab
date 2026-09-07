#include "core/slice.h"

#include "api/library.h"
#include "gab.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char SLICE_SRC[] = "interface Index<T> {\n"
                                "    func index(self: &Self, at: i32): &T;\n"
                                "}\n"
                                "impl<T> slice<T> {\n"
                                "    intrinsic func len(self: &slice<T>): i32;\n"
                                "}\n"
                                "impl<T> slice<T> as Index<T> {\n"
                                "    intrinsic func index(self: &slice<T>, at: i32): &T;\n"
                                "}\n"

                                "impl<T, N: i32> array<T, N> {\n"
                                "    intrinsic func len(self: &Self): i32;\n"
                                "}\n"
                                "impl<T, N: i32> array<T, N> as Index<T> {\n"
                                "    intrinsic func index(self: &Self, at: i32): &T;\n"
                                "}\n";

void core_register_slice(VM *vm) {
    GabError err;
    GabLib *core = library_open_prelude(vm, GAB_CORE_MODULE);

    bool loaded = gab_lib_source(core, SLICE_SRC, &err);

    assert(loaded && "a library's declarations compile");
    (void)loaded;

    gab_lib_close(core);
}
