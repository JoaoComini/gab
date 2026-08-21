#ifndef GAB_OBJECT_H
#define GAB_OBJECT_H

#include "allocator.h"
#include "type.h"

/*
    A heap object is a header immediately followed by its payload, and a '*T'
    is the address of the *payload* — never of the header. That is what makes a
    heap pointer byte-identical to a stack one: every opcode that moves, loads,
    or stores a pointer works on both without knowing which it has, and only
    ownership tells them apart.

    The header is found by walking back one:  header = (ObjectHeader *)ptr - 1.
*/

/*
    A 'string' value: where the characters are and how many there are. Passed
    and copied by value like a small struct, and owning nothing -- the
    characters belong to whatever allocated them, which for a literal is the
    unit's arena.

    Laid out to tile the stack exactly, so a string in a frame slot and a string
    in a struct field are the same bytes.
*/
typedef struct {
    const char *data;
    int32_t length;
} GabStringValue;

typedef struct ObjectHeader {
    // What the payload is, and the only thing the header carries. Freeing a
    // Player means freeing the objects its '*T' fields own, and only the Type
    // says where those are. A Type is pointer-stable and outlives every
    // compile, so holding one costs nothing.
    //
    // There is no reference count. Ownership is unique and statically known:
    // exactly one slot owns an object, and codegen frees it where that slot
    // goes out of scope. A count would only ever have read 1.
    const Type *type;
} ObjectHeader;

// The payload must land at its own alignment, so the header's size has to be a
// multiple of the widest alignment a Gab type can ask for. That is 8 today,
// from a pointer field. A wider type later fails this rather than silently
// misaligning every object of it.
_Static_assert(sizeof(ObjectHeader) % 8 == 0, "the header must not misalign the payload that follows it");

// The header of a payload address. Not for a stack pointer: there is no header
// there, and nothing in the representation can tell the caller so.
ObjectHeader *gab_object_of(void *payload);

// Allocates a zeroed payload of 'type' and returns it. NULL if the allocation
// fails.
void *gab_object_alloc(Allocator allocator, const Type *type);

// Frees a payload and everything it owns. NULL-tolerant, because a '*T' that
// was never assigned is NULL and every free path would otherwise need the same
// guard.
//
// Freeing a tree of objects recurses, which is O(depth) in C stack for a deep
// one. A 'ref T' field is not followed: it never owned what it names.
void gab_object_free(Allocator allocator, void *payload);

#endif
