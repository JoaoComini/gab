#include "driver/link.h"

#include "scope.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static bool readable(const char *path);

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

    char self[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);

    if (length <= 0) {
        snprintf(directory, sizeof(directory), ".");
        return directory;
    }

    self[length] = '\0';

    char *slash = strrchr(self, '/');

    if (slash) {
        *slash = '\0';
    }

    snprintf(directory, sizeof(directory), "%s/../lib/gab", self);

    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s/%s.gabi", directory, GAB_CORE_MODULE);

    if (readable(probe)) {
        return directory;
    }

    snprintf(directory, sizeof(directory), "%s", self);

    return directory;
}

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

static bool readable(const char *path) {
    FILE *file = fopen(path, "rb");

    if (!file) {
        return false;
    }

    fclose(file);

    return true;
}

void gab_object_beside(const char *interface, char *out, size_t capacity) {
    size_t length = strlen(interface);

    if (length > 5 && strcmp(interface + length - 5, ".gabi") == 0) {
        length -= 5;
    }

    snprintf(out, capacity, "%.*s.o", (int)length, interface);
}

bool gab_link(const char *object, const char *module, const char *const *extra, size_t extra_count,
              const char *binary) {
    const char *cc = getenv("GABC_CC");

    if (!cc || !*cc) {
        cc = "cc";
    }

    char probe[512];
    snprintf(probe, sizeof(probe), "command -v %s > /dev/null 2>&1", cc);

    if (system(probe) != 0) {
        fprintf(stderr,
                "gabc: '%s' is not installed, and linking needs it; set GABC_CC to a C compiler, or "
                "pass '-c' to compile without linking\n",
                cc);

        return false;
    }

    const char *libdir = gab_libdir();

    const char *flags = getenv("GABC_LINK_FLAGS");

    char entry[PATH_MAX] = {0};

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

    snprintf(command + length, sizeof(command) - length, " %s/libcore.a %s -o %s", libdir, flags ? flags : "",
             binary);

    char captured[512];
    snprintf(captured, sizeof(captured), "%s/gab.link.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
             (int)getpid());

    size_t at = strlen(command);
    snprintf(command + at, sizeof(command) - at, " 2>%s", captured);

    bool linked = system(command) == 0;

    FILE *report = fopen(captured, "r");

    if (report) {
        char line[1024];
        bool stale = false;

        while (fgets(line, sizeof(line), report)) {
            char *marker = strstr(line, "gab.iface.");

            if (!marker) {
                fputs(line, stderr);
                continue;
            }

            char module[128];

            if (sscanf(marker, "gab.iface.%127[^.]", module) == 1) {
                bool present = false;

                for (size_t i = 0; i < extra_count; i++) {
                    const char *slash = strrchr(extra[i], '/');
                    const char *base = slash ? slash + 1 : extra[i];

                    present = present || strncmp(base, module, strlen(module)) == 0;
                }

                if (present) {
                    fprintf(stderr,
                            "the interface for '%s' is not the one its object was compiled from: "
                            "recompile them together\n",
                            module);
                } else {
                    fprintf(stderr, "no object for module '%s' was linked\n", module);
                }

                stale = true;
            }
        }

        fclose(report);
        remove(captured);

        (void)stale;
    }

    return linked;
}
