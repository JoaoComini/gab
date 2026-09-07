#ifndef GAB_SLOT_H
#define GAB_SLOT_H

#define VM_SLOT_SIZE 4

#include "type/type_registry.h"

/* How many slots a value of this type occupies, which is what a frame reserves for it. */
static inline unsigned int args_type_slots(TypeRegistry *registry, const Type *type) {
    if (!type) {
        return 1;
    }

    return (unsigned int)((type_registry_size_of(registry, type) + VM_SLOT_SIZE - 1) / VM_SLOT_SIZE);
}

#endif
