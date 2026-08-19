#ifndef GAB_REFCOUNTED_H
#define GAB_REFCOUNTED_H

#include "allocator.h"
#include "type.h"

#include <stdint.h>

/*
    A heap object is a header immediately followed by its payload, and a '*T'
    is the address of the *payload* — never of the header. That is what makes a
    heap pointer byte-identical to a stack one: every opcode that moves, loads,
    or stores a pointer works on both without knowing which it has, and only
    ownership tells them apart.

    The header is found by walking back one:  header = (RefCounted *)ptr - 1.
*/
typedef struct RefCounted {
    // Non-atomic: Gab is single-threaded, and making these atomic would cost
    // every retain and release for a guarantee nothing needs. Deliberate, not
    // an oversight.
    uint32_t strong;

    // A separate count rather than a flag, so the header can outlive the
    // payload: a weak reference has to stay dereferenceable long enough to
    // observe that its pointee is gone. Nothing creates weak references yet —
    // the field is here so that adding 'weak *T' needs no layout change.
    uint32_t weak;

    // What the payload is. Release needs it: freeing a Player means releasing
    // the '*T' fields inside the Player, and only the Type says where those
    // are. A Type is pointer-stable and outlives every compile, so holding one
    // costs nothing.
    const Type *type;
} RefCounted;

// The payload must land at its own alignment, so the header's size has to be a
// multiple of the widest alignment a Gab type can ask for. That is 8 today,
// from a pointer field. A wider type later fails this rather than silently
// misaligning every object of it.
_Static_assert(sizeof(RefCounted) % 8 == 0, "the header must not misalign the payload that follows it");

// The header of a payload address. Not for a stack pointer: there is no header
// there, and nothing in the representation can tell the caller so.
RefCounted *gab_refcounted_of(void *payload);

// Allocates a zeroed payload of 'type' with a header owning one strong
// reference, and returns the payload. NULL if the allocation fails.
void *gab_refcounted_alloc(Allocator allocator, const Type *type);

// Both are NULL-tolerant, because a '*T' that was never assigned is NULL and
// every release path would otherwise need the same guard.
void gab_retain(void *payload);

// Drops one strong reference, and at zero releases the pointer fields the
// payload holds and zeroes it. Freeing a tree of objects therefore recurses,
// which is O(depth) in C stack for a deep one.
//
// The header is freed with the payload only when nothing holds a weak
// reference; otherwise it stays behind, with strong == 0 recording that the
// payload is gone, until the last weak reference goes.
void gab_release(Allocator allocator, void *payload);

// A weak reference does not keep its object alive. It exists so that a cycle —
// a child pointing back at its parent, an entity at the world holding it, which
// is the ordinary shape of game data — does not leak both ends.
//
// Both are NULL-tolerant, as the strong pair is.
void gab_retain_weak(void *payload);
void gab_release_weak(Allocator allocator, void *payload);

// Whether the payload behind a weak reference is still there. False once the
// last strong reference went, even though the address is still readable: that
// is precisely what the surviving header is for.
bool gab_is_alive(const void *payload);

#endif
