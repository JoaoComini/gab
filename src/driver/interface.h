#ifndef GAB_DRIVER_INTERFACE_H
#define GAB_DRIVER_INTERFACE_H

#include "ast/ast.h"
#include "ast/facts.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* A compiled unit's declarations as Gab source with no bodies: what a later compilation reads in place
 * of the source it was built from. Reading one back is parsing, so the format cannot drift from the
 * language it describes. */
/* The interface text for a unit, which is the source a reader parses to know what this module declares. */
void gab_interface_print(const ASTUnit *unit, const Facts *facts, FILE *out);

bool gab_interface_write(const ASTUnit *unit, const Facts *facts, const char *path);

/* One written earlier, as text to compile; null where none is there. The caller frees it. */
char *gab_interface_read(const char *path);

/* What an interface's declarations hash to, which an object records so a stale pairing cannot link. */
uint64_t gab_interface_digest(const char *text);

/* The symbol standing for 'module' as this interface declares it, written into 'out'. */
void gab_interface_symbol(char *out, size_t capacity, const char *module, uint64_t digest);

#endif
