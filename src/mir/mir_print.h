#ifndef GAB_MIR_PRINT_H
#define GAB_MIR_PRINT_H

#include "mir/mir.h"
#include "type/type_registry.h"

#include <stdio.h>

void mir_print(const MIRFunction *ir, TypeRegistry *registry, FILE *out);

/* Writes the same text into a caller's buffer, so a test can assert on it without a temporary file. */
size_t mir_print_to_buffer(const MIRFunction *ir, TypeRegistry *registry, char *buffer, size_t capacity);

#endif
