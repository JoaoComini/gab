#ifndef GAB_TYPE_LAYOUT_H
#define GAB_TYPE_LAYOUT_H

// What follows from a type rather than what a type is: where its value sits in
// memory, and what freeing one has to do. Both are derived where a type is laid
// out and read back through the registry that owns them.

#include "type.h"

#include <stddef.h>
#include <stdint.h>

/*
    What freeing one value of a type has to do, with every offset it needs
    already in it.

    Built once, where the layout is computed, and read by the free path alone.
    Baking the offsets in is what keeps a laid-out Type out of the free path: a
    walk that read them back off the type would make layout something the VM
    consults on every free, rather than a fact the compiler settles and spends.

    The shape a plan takes is the shape of what owns, not the shape of the type:
    a struct of forty fields of which one owns is a plan of one step. A type
    that owns nothing has no plan at all, and the free path tests for that
    rather than walking a plan to learn there is nothing to do.
*/
typedef struct DropPlan DropPlan;

typedef enum {
    // Free what the address at this offset names, then the plan of the pointee.
    // What 'new box T' allocates and what every owning pointer field holds.
    DROP_BOX,

    // Free the block an owning address names, at the capacity beside it. What
    // a string's characters are, for an element of any width.
    //
    // What the live elements own is freed first, then the memory: a block
    // carries both numbers, so one step does the whole of it.
    DROP_BLOCK,

    // Walk a run of elements, freeing what each owns. Strides by a width the
    // plan carries rather than by one read back off a type.
    DROP_ARRAY,

    // Run each step at its own offset. What a struct is, and the only kind
    // whose offsets a plan has to carry.
    DROP_FIELDS,
} DropKind;

// One thing a plan does, at a fixed offset into the value. The offset is
// absolute within the value the plan describes, so a walk adds it and recurses
// rather than accumulating a base.
typedef struct DropStep {
    size_t offset;
    const DropPlan *plan;
} DropStep;

struct DropPlan {
    DropKind kind;

    // DROP_BOX: what the pointee owns, or NULL when it owns nothing and freeing
    // the block is the whole of it.
    // DROP_BLOCK: what one element owns, or NULL when the memory is all there
    // is to free.
    // DROP_ARRAY: what one element owns, which is why the run is walked at all.
    // DROP_FIELDS: unused; the steps carry the plans.
    const DropPlan *inner;

    // DROP_ARRAY, DROP_BLOCK: how far apart the elements are.
    size_t stride;

    // DROP_ARRAY: how many there are, which its type says. The other two count
    // at run time -- a block by the capacity it carries, a prefix by the field
    // named below.
    int32_t length;

    // DROP_FIELDS: what owns, and where. Only the fields that own appear.
    const DropStep *steps;
    size_t step_count;
};

/*
    Where a value of a type sits in memory: how wide it is, what it must be
    aligned to, and where each of its fields begins.

    Beside the types rather than on them, for the reason a method set and a drop
    plan are: what a type is was settled when it was interned, while how wide it
    is follows from what its parts are laid out as. Keeping them apart is what
    lets a generic answer its layout per instantiation without the type itself
    being rebuilt -- and what lets a type be finished when the registry hands it
    over.

    Read through the registry, which is what owns one. See
    type_registry_layout_of.
*/
typedef struct TypeLayout {
    size_t size;
    size_t alignment;

    // Where each field begins, in the order the type declares them. Indexed by
    // the same i that indexes type_fields, so a walk over the two reads one
    // field's name and its offset from the same position.
    //
    // NULL for a kind with no fields, which is the right no-op for a walk that
    // has no fields to make.
    const size_t *offsets;
    size_t offset_count;
} TypeLayout;

#endif
