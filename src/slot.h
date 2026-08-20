#ifndef GAB_SLOT_H
#define GAB_SLOT_H

// The unit the stack is addressed in. A slot is four bytes and carries no tag:
// static types already say what a slot holds, and untagged slots are what let a
// struct spread over consecutive slots be byte-identical to the equivalent C
// struct -- which is what lets a host pass one in and read one back.
//
// A slot is a size, not a type. Nothing is "a slot" the way something is an
// int: an int and a float each fill one, a pointer tiles over two, and a struct
// covers as many as its layout needs. Reads and writes go through the accessors
// in vm.h, each of which names the width it moves.
#define VM_SLOT_SIZE 4

#endif
