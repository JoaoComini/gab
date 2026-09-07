#include "core/str.h"

#include "api/library.h"
#include "gab.h"
#include "scope.h"

#include <assert.h>

static const char CORE_SRC[] = "impl str {\n"
                               "    intrinsic func as_bytes(self: &str): &slice<byte>;\n"
                               "    func len(self: &str): int { return self.as_bytes().len(); }\n"
                               "}\n";

void core_register_str(VM *vm) {
    GabError err;
    GabLib *core = library_open_prelude(vm, GAB_CORE_MODULE);

    bool loaded = gab_lib_source(core, CORE_SRC, &err);

    assert(loaded && "a library's declarations compile");
    (void)loaded;

    gab_lib_close(core);
}
