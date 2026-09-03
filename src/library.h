#ifndef GAB_LIBRARY_H
#define GAB_LIBRARY_H

#include "gab.h"
#include "vm/vm.h"

/* The prelude's scope is the global one, and only it may impl a primitive. */
GabLib *library_open_prelude(VM *vm, const char *module);

#endif
