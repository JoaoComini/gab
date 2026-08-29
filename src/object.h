#ifndef GAB_OBJECT_H
#define GAB_OBJECT_H

#include "allocator.h"
#include "arena.h"
#include "type/type.h"
#include "type/type_layout.h"

typedef struct TypeRegistry TypeRegistry;

/*
    A heap object is a header immediately followed by its payload, and a 'box T'
    is the address of the *payload* — never of the header. That is what makes a
    heap pointer byte-identical to a stack one: every opcode that moves, loads,
    or stores a pointer works on both without knowing which it has, and only
    ownership tells them apart.

    The header is found by walking back one:  header = (ObjectHeader *)ptr - 1.
*/

/*
    A 'block T' value: where the elements are, how many the memory has room for,
    and how many of them are live.

    Everything freeing it needs, which is what makes a block the one owning
    shape that answers for itself. The capacity is what the memory is freed at;
    the length is how far into it anything was written, and the two are
    different questions -- memory has no mark telling a written element from a
    zeroed one.

    The length rides in what the capacity's alignment already padded out, so
    carrying it costs nothing: a block is two words either way.

    Self-sufficiency is the point. A count kept in a sibling field would make
    this field owning without being freeable alone, and every walk that descends
    into a value would have to be told to stop here.
*/
typedef struct {
    void *data;
    int32_t capacity;
    int32_t length;
} GabBlockValue;

/*
    A 'String' value: the block holding its characters, which carries how many
    there are.

    One field, because a block answers for itself -- what a string adds to it is
    that the characters are text, not a number the block was missing. A
    'Vec<T>' is the same shape with a different element.
*/
typedef struct {
    GabBlockValue block;
} GabStringValue;

/*
    What a 'ref str' is: where the characters are and how many of them the
    reference names.

    Its own type rather than the header's, though the two are the same words
    today. A 'str' is the characters themselves and has no width, so what a
    reference to it carries is not read off the pointee -- it is this, and a
    header lends it by handing over the fields it names.

    What the two are is what separates them: a header owns its characters and
    may grow fields of its own, while a reference names someone else's and never
    carries more than it takes to find them.
*/
typedef struct {
    const char *data;
    int32_t length;
} GabStrRef;

/*
    An '[T; N]' value: where the elements are and how many. The same two slots
    a string has, and copied the same way -- what differs is that the element is
    whatever the array was written over, which is what its drop walk reads.
*/
typedef struct {
    void *data;
    int32_t length;
} GabArrayValue;

typedef struct ObjectHeader {
    // What freeing the payload has to do, and the only thing the header
    // carries. A plan rather than the type it came from: freeing needs the
    // offsets of what the payload owns and nothing else about it, and the plan
    // is those offsets. A plan is pointer-stable and outlives every compile, so
    // holding one costs nothing.
    //
    // There is no reference count. Ownership is unique and statically known:
    // exactly one slot owns an object, and codegen frees it where that slot
    // goes out of scope. A count would only ever have read 1.
    const DropPlan *drop;
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

/*
    Growing a block, which is what every collection built on one does to make
    room. Here rather than in each of them: a string and a vector reserve the
    same way and differ only in what one element is, so 'stride' is the whole of
    what they do not share.
*/

// Makes room for 'extra' more elements past the live ones, growing the block if
// they do not fit. Doubling, so that appending n elements copies O(n) times
// rather than once per append.
//
// False when the allocation would overflow or fails, leaving the block as it
// was: the caller must not then write what it was making room for.
bool block_reserve(Allocator allocator, GabBlockValue *block, int32_t extra, size_t stride);

// Builds the drop plan for a laid-out type, or NULL when it owns nothing.
// Derived once per type and never on a free path, which is what lets the plan
// bake in offsets the free path would otherwise read back off the type.
//
// Not called directly: type_registry_drop_of memoizes this, and the recursion
// below goes through it so that a ring through a 'box' terminates.
const DropPlan *object_build_drop(Arena *arena, TypeRegistry *registry, const Type *type);

// The header of a payload address. Not for a stack pointer: there is no header
// there, and nothing in the representation can tell the caller so.
ObjectHeader *object_of(void *payload);

// Allocates a zeroed payload of 'size' bytes, carrying the plan that frees what
// it owns. NULL if the allocation fails.
void *object_alloc(Allocator allocator, size_t size, const DropPlan *drop);

// Frees what a value of 'type' sitting at 'value' owns, and the value itself
// where it is an owning pointer. This is what a release does: the type is known
// where the slot goes out of scope, so the free never has to rediscover it.
//
// Takes the slot's address rather than the object's, because not every owning
// value is a pointer -- an array is a header, and freeing it needs the length
// beside the pointer. NULL-tolerant through each type's own drop.
void object_release(Allocator allocator, const DropPlan *drop, void *value);

// Frees a payload and everything it owns. NULL-tolerant, because a 'box T' that
// was never assigned is NULL and every free path would otherwise need the same
// guard.
//
// Freeing a tree of objects recurses, which is O(depth) in C stack for a deep
// one. What is followed is decided by the type's drop function, chosen when its
// layout was: a 'ref' has none, having never owned what it names.
void object_free(Allocator allocator, void *payload);

#endif
