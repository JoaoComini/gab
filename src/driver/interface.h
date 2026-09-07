#ifndef GAB_DRIVER_INTERFACE_H
#define GAB_DRIVER_INTERFACE_H

#include "ast/ast.h"

#include <stdbool.h>
#include <stdio.h>

/* A compiled unit's declarations as Gab source with no bodies: what a later compilation reads in place
 * of the source it was built from. Reading one back is parsing, so the format cannot drift from the
 * language it describes. */
bool gab_interface_write(const ASTUnit *unit, const char *path);

/* One written earlier, as text to compile; null where none is there. The caller frees it. */
char *gab_interface_read(const char *path);

#endif
