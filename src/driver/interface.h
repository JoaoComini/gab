#ifndef GAB_DRIVER_INTERFACE_H
#define GAB_DRIVER_INTERFACE_H

#include "ast/ast.h"
#include "ast/facts.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

void gab_interface_print(const ASTModule *module, const Facts *facts, FILE *out);

bool gab_interface_write(const ASTModule *module, const Facts *facts, const char *path);

char *gab_interface_read(const char *path);

uint64_t gab_interface_digest(const char *text);

void gab_interface_symbol(char *out, size_t capacity, const char *module, uint64_t digest);

#endif
