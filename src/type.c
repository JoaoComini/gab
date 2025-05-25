#include "type.h"
#include <stdlib.h>

TypeSpec *type_spec_create(StringRef name) {
    TypeSpec *spec = malloc(sizeof(TypeSpec));
    spec->name = name;

    return spec;
}

void type_spec_free(TypeSpec *spec) { free(spec); }
