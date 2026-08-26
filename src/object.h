#ifndef GAB_OBJECT_H
#define GAB_OBJECT_H

#include "allocator.h"
#include "type.h"

/*
    A heap object is a header immediately followed by its payload, and a 'box T'
    is the address of the *payload* — never of the header. That is what makes a
    heap pointer byte-identical to a stack one: every opcode that moves, loads,
    or stores a pointer works on both without knowing which it has, and only
    ownership tells them apart.

    The header is found by walking back one:  header = (ObjectHeader *)ptr - 1.
*/

/*
    A 'String' value: where the characters are and how many there are. Passed
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

/*
    An 'Array T' value: where the elements are and how many. The same two slots
    a string has, and copied the same way -- what differs is that the element is
    whatever the array was written over, which is what its drop walk reads.
*/
typedef struct {
    void *data;
    int32_t length;
} GabArrayValue;

typedef struct ObjectHeader {
    // How many elements the payload holds, for a block one of the two header
    // types names: an array's elements or a string's characters. One for every
    // other object, which is the single value its type describes.
    //
    // Here rather than on the array's own header because this is what freeing
    // reads: 'OP_RELEASE' is handed the block's address and nothing else, so
    // the count of live elements has to travel with the block.
    size_t count;

    // What the payload is, and the first thing the header carries. Freeing a
    // Player means running the drop function its Type holds, which is where the
    // offsets of everything it owns live. A Type is pointer-stable and outlives
    // every compile, so holding one costs nothing.
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

// An array's byte count is its int32_t length times its element's width,
// computed in size_t. That product cannot wrap while a size_t is this wide, so
// allocating an array never has to check for it.
_Static_assert(sizeof(size_t) >= 8, "an array's size computation assumes a 64-bit size_t");

// Selects the drop function for a laid-out type, or NULL when it owns nothing.
// Called wherever a Type is finished -- once per type, never on a free path.
void object_select_drop(Type *type);

// The header of a payload address. Not for a stack pointer: there is no header
// there, and nothing in the representation can tell the caller so.
ObjectHeader *object_of(void *payload);

// Allocates a zeroed payload of 'type' and returns it. NULL if the allocation
// fails.
void *object_alloc(Allocator allocator, const Type *type);

// As object_alloc, for a payload whose size is not the type's: a string's
// characters and an array's elements are counted at run time, so 'size' says
// how many bytes follow the header rather than the type doing.
//
// 'count' is how many elements those bytes hold, which freeing reads to know
// how far to walk. A block of characters owns nothing and passes its byte
// count; what matters there is only that the walk finds no dropper.
void *object_alloc_sized(Allocator allocator, const Type *type, size_t size, size_t count);

// Frees a payload and everything it owns. NULL-tolerant, because a 'box T' that
// was never assigned is NULL and every free path would otherwise need the same
// guard.
//
// Freeing a tree of objects recurses, which is O(depth) in C stack for a deep
// one. What is followed is decided by the type's drop function, chosen when its
// layout was: a 'ref' has none, having never owned what it names.
void object_free(Allocator allocator, void *payload);

#endif
