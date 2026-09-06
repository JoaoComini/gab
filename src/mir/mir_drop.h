#ifndef GAB_MIR_DROP_H
#define GAB_MIR_DROP_H

#include "memory/arena.h"
#include "mir/mir.h"
#include "type/type_registry.h"

/* Nulls what a move gave away, so a value moved on one path and not another reaches its drop holding
 * nothing on the path that gave it away, and the release frees nothing rather than freeing twice. */
void mir_drop_elaborate(Arena *arena, TypeRegistry *registry, MIRFunction *ir);

#endif
