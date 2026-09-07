#include "driver/link.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *gab_libdir(void) {
    static char directory[PATH_MAX];

    if (directory[0]) {
        return directory;
    }

    const char *override = getenv("GABC_LIBDIR");

    if (override && *override) {
        snprintf(directory, sizeof(directory), "%s", override);
        return directory;
    }

    ssize_t length = readlink("/proc/self/exe", directory, sizeof(directory) - 1);

    if (length <= 0) {
        snprintf(directory, sizeof(directory), ".");
        return directory;
    }

    directory[length] = '\0';

    char *slash = strrchr(directory, '/');

    if (slash) {
        *slash = '\0';
    }

    return directory;
}

/* What 'crt1.o' calls: a program is entered at the C 'main', which calls the module's. The shim is
 * written rather than emitted so the entry point stays the driver's decision. */
static bool write_entry(const char *path, const char *module) {
    FILE *file = fopen(path, "w");

    if (!file) {
        return false;
    }

    fprintf(file,
            "extern int gab_main(void) __asm__(\"%s.main\");\n"
            "int main(void) { return gab_main(); }\n",
            module);

    fclose(file);

    return true;
}

bool gab_link(const char *object, const char *module, const char *const *extra, size_t extra_count,
              const char *binary) {
    const char *cc = getenv("GABC_CC");

    if (!cc || !*cc) {
        cc = "cc";
    }

    const char *libdir = gab_libdir();

    /* The sanitizers a program links must match those its runtime was built with. */
    const char *flags = getenv("GABC_LINK_FLAGS");

    char entry[PATH_MAX] = {0};

    /* Only a C source supplies an entry point; an object linked beside this one does not. */
    bool entry_given = false;

    for (size_t i = 0; i < extra_count; i++) {
        const char *dot = strrchr(extra[i], '.');

        entry_given = entry_given || (dot && strcmp(dot, ".c") == 0);
    }

    if (!entry_given) {
        snprintf(entry, sizeof(entry), "%s.entry.c", binary);

        if (!write_entry(entry, module)) {
            return false;
        }
    }

    char command[4096];
    size_t length = (size_t)snprintf(command, sizeof(command), "%s %s %s", cc, object, entry);

    for (size_t i = 0; i < extra_count && length < sizeof(command); i++) {
        length += (size_t)snprintf(command + length, sizeof(command) - length, " %s", extra[i]);
    }

    snprintf(command + length, sizeof(command) - length, " %s/libcore.a %s/libgab_runtime.a %s -o %s", libdir,
             libdir, flags ? flags : "", binary);

    return system(command) == 0;
}
