#include "builtin/builtin.h"

#include "object.h"
#include "type_registry.h"
#include "vm/args.h"
#include "vm/interp.h"
#include "vm/vm.h"

#include "allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

const DropPlan *string_build_drop(TypeRegistry *registry, const Type *type) {
    // 'length' counts what has been written into 'data'. A character owns
    // nothing, so no prefix is walked and the block's own free is the whole of
    // what a string has to do -- but the composition is the one a vector takes,
    // since what differs between them is the element rather than how one frees.
    return drop_plan_counted_block(type_registry_arena(registry), registry, type, 1, 0);
}

// Where 'needle' first occurs in 'haystack' at or after 'from', or -1. The
// empty needle occurs at the position asked for, which is what makes
// 'index_of("")' zero.
static int32_t string_find(GabStrRef haystack, GabStrRef needle, int32_t from) {
    // Checked before the loop bound is computed: the lengths are unsigned, so a
    // needle longer than the haystack would wrap the subtraction into a huge
    // bound rather than an empty range.
    if (needle.length > haystack.length) {
        return -1;
    }

    for (int32_t start = from; start <= (int32_t)(haystack.length - needle.length); start++) {
        if (memcmp(haystack.data + start, needle.data, needle.length) == 0) {
            return start;
        }
    }

    return -1;
}

// 's.len()'. Reads the count the receiver's header already carries.
static void string_len(Args *args) { args_return_int(args, args_string(args, 0).length); }

// 's.is_empty()'. Whether the receiver denotes any character at all.
static void string_is_empty(Args *args) { args_return_bool(args, args_string(args, 0).length == 0); }

// 's.at(i)'. The character at an index, as its numeric value.
//
// An index outside the string fails the run rather than answering: there is no
// value that could mean 'no character' without a caller mistaking it for one.
static void string_at(Args *args) {
    GabStrRef string = args_string(args, 0);
    int32_t index = args_int(args, 1);

    if (index < 0 || (size_t)index >= (size_t)string.length) {
        vm_fail(args->vm, VM_RUN_ERR_EXTERN, "string index is out of range");
        return;
    }

    args_return_int(args, (unsigned char)string.data[index]);
}

// 's.starts_with(p)'. Whether the receiver's leading characters spell 'p'.
//
// The length is checked before the comparison rather than left to it: a prefix
// longer than the receiver would otherwise be read past its end, and the
// characters there belong to whatever the arena interned next -- so the answer
// would depend on that rather than on the two strings.
static void string_starts_with(Args *args) {
    GabStrRef string = args_string(args, 0);
    GabStrRef prefix = args_string(args, 1);

    args_return_bool(args,
                     prefix.length <= string.length && memcmp(string.data, prefix.data, prefix.length) == 0);
}

// 's.ends_with(p)'. Whether the receiver's trailing characters spell 'p'. The
// length is checked first for the reason 'starts_with' gives, and here it also
// keeps the subtraction below from wrapping.
static void string_ends_with(Args *args) {
    GabStrRef string = args_string(args, 0);
    GabStrRef suffix = args_string(args, 1);

    args_return_bool(args,
                     suffix.length <= string.length && memcmp(string.data + (string.length - suffix.length),
                                                              suffix.data, suffix.length) == 0);
}

// 's.contains(o)'. Whether 'o' occurs anywhere in the receiver.
static void string_contains(Args *args) {
    args_return_bool(args, string_find(args_string(args, 0), args_string(args, 1), 0) >= 0);
}

// 's.index_of(o)'. Where 'o' first occurs, or -1 when it does not.
static void string_index_of(Args *args) {
    args_return_int(args, string_find(args_string(args, 0), args_string(args, 1), 0));
}

// 's.count(o)'. How many non-overlapping occurrences of 'o' the receiver holds:
// a match resumes past the one just found, so 'aaa' holds one 'aa'.
static void string_count(Args *args) {
    GabStrRef string = args_string(args, 0);
    GabStrRef needle = args_string(args, 1);

    // The empty needle matches at every position without consuming one, so the
    // loop below would never advance past it.
    if (needle.length == 0) {
        args_return_int(args, 0);
        return;
    }

    int32_t total = 0;

    for (int32_t at = 0; (at = string_find(string, needle, at)) >= 0; at += (int32_t)needle.length) {
        total++;
    }

    args_return_int(args, total);
}

// What an empty string's first allocation holds. Small enough that a string
// pushed into once does not reserve a page, large enough that the first few
// pushes do not each reallocate.
#define STRING_INITIAL_CAPACITY 8

// Makes room for 'extra' more characters, growing the block if the live ones
// leave too little. Doubling, so that appending n characters copies O(n) times
// rather than once per push.
//
// False when the allocation fails, having failed the run: the caller must not
// then write the characters it was making room for.
static bool string_reserve(Args *args, GabStringValue *string, int32_t extra) {
    if (string->length + extra <= string->block.capacity) {
        return true;
    }

    // Doubling overflows a signed int past 2^30 characters, which is undefined
    // rather than merely wrong. A string that large has no room to grow, so the
    // run fails here rather than wrapping to a smaller block than it holds.
    if (string->length > INT32_MAX - extra || string->block.capacity > INT32_MAX / 2) {
        vm_fail(args->vm, VM_RUN_ERR_OUT_OF_MEMORY, "a string cannot grow any further");
        return false;
    }

    int32_t capacity = string->block.capacity ? string->block.capacity * 2 : STRING_INITIAL_CAPACITY;

    // Doubling may still not reach what was asked for, since 'extra' is not
    // bounded by the capacity: a long append onto a short string clears it in
    // one step rather than looping.
    if (capacity < string->length + extra) {
        capacity = string->length + extra;
    }

    char *block = DEFAULT_ALLOCATOR.alloc(DEFAULT_ALLOCATOR.ctx, (size_t)capacity);

    if (!block) {
        vm_fail(args->vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory growing a string");
        return false;
    }

    if (string->length) {
        memcpy(block, string->block.data, (size_t)string->length);
    }

    if (string->block.data) {
        DEFAULT_ALLOCATOR.free_sized(DEFAULT_ALLOCATOR.ctx, string->block.data,
                                     (size_t)string->block.capacity);
    }

    string->block.data = block;
    string->block.capacity = capacity;

    return true;
}

// 's.push(c)'. Writes one character past the live ones, growing first if there
// is no room.
static void string_push(Args *args) {
    GabStringValue string = args_string_at(args, 0);
    int32_t character = args_int(args, 1);

    if (!string_reserve(args, &string, 1)) {
        return;
    }

    ((char *)string.block.data)[string.length] = (char)character;
    string.length++;

    memcpy(args_pointer(args, 0), &string, sizeof(string));
}

// 's.append(o)'. Spells the characters of 'o' after the receiver's own.
//
// The source is read before the reserve may move the block, since a string
// appended to itself would otherwise copy out of freed memory.
static void string_append(Args *args) {
    GabStringValue string = args_string_at(args, 0);
    GabStrRef other = args_string(args, 1);

    if (other.length == 0) {
        return;
    }

    // Copied out first for the reason above: 'other' may name the very block
    // the reserve below frees.
    char *copy = DEFAULT_ALLOCATOR.alloc(DEFAULT_ALLOCATOR.ctx, (size_t)other.length);

    if (!copy) {
        vm_fail(args->vm, VM_RUN_ERR_OUT_OF_MEMORY, "out of memory appending to a string");
        return;
    }

    memcpy(copy, other.data, (size_t)other.length);

    if (string_reserve(args, &string, other.length)) {
        memcpy((char *)string.block.data + string.length, copy, (size_t)other.length);
        string.length += other.length;

        memcpy(args_pointer(args, 0), &string, sizeof(string));
    }

    DEFAULT_ALLOCATOR.free_sized(DEFAULT_ALLOCATOR.ctx, copy, (size_t)other.length);
}

// 's.to_owned()'. The characters a borrow names, copied into a string that owns
// them. The one string method that allocates, and the way anything arena-backed
// -- a literal, or the join of two -- becomes something a 'String' slot may
// hold.
static void string_to_owned(Args *args) {
    GabStrRef string = args_string(args, 0);

    args_return_string_copy(args, string.data, string.length);
}

// 's.clone()'. What 'to_owned' does, for the owning half: duplicating what a
// 'String' holds and copying what a 'str' names are the same allocation. The
// two differ only in how the receiver reaches the body, which is what the
// declaration says and what each reads through.
static void string_clone(Args *args) {
    GabStringValue string = args_string_at(args, 0);

    args_return_string_copy(args, string.block.data, string.length);
}

// 'String::from(s)'. The characters a borrow names, copied into a string that
// owns them -- what 's.to_owned()' does, reached on the type rather than on the
// characters. The two spellings suit different call sites: a constructor where
// the source is an expression, a method where it is already a receiver.
static void string_from(Args *args) {
    GabStrRef string = args_string(args, 0);

    args_return_string_copy(args, string.data, string.length);
}

void builtin_register_string(VM *vm) {
    TypeRegistry *registry = vm->env.global_scope.type_registry;

    // Every method here reads its receiver and allocates nothing, so it takes a
    // borrow: an owning string lends to one, and a literal already is one.
    //
    // Declared on the owning type even so, because that is the set both halves
    // read: a borrow reaches it through 'owner', while the owning string has no
    // route to the borrow's own set. What each takes stays the borrow, so a
    // 'String' receiver lends where the parameter says 'str'.
    const Type *string_type = registry->builtins.string_type;

    // What every method takes and what a literal is: characters named by a
    // reference that carries how many. The owning string lends one.
    const Type *str_type = type_registry_ref_to(registry, registry->builtins.str_type);

    // What 'clone' takes. A receiver by value would have to copy it, which an
    // owning string cannot do -- the rule a script's own method obeys -- so it
    // borrows the slot the header sits in.
    const Type *ref_string = type_registry_ref_to(registry, string_type);

    const Type *int_type = registry->builtins.int_type;
    const Type *bool_type = registry->builtins.bool_type;

    const Type *const int_param[] = {int_type};
    const Type *const string_param[] = {str_type};

    builtin_register_method(vm, registry->builtins.str_type, str_type, "len", string_len, int_type, NULL, 0);
    builtin_register_method(vm, registry->builtins.str_type, str_type, "is_empty", string_is_empty, bool_type,
                            NULL, 0);
    builtin_register_method(vm, registry->builtins.str_type, str_type, "at", string_at, int_type, int_param,
                            1);
    builtin_register_method(vm, registry->builtins.str_type, str_type, "starts_with", string_starts_with,
                            bool_type, string_param, 1);
    builtin_register_method(vm, registry->builtins.str_type, str_type, "ends_with", string_ends_with,
                            bool_type, string_param, 1);
    builtin_register_method(vm, registry->builtins.str_type, str_type, "contains", string_contains, bool_type,
                            string_param, 1);
    builtin_register_method(vm, registry->builtins.str_type, str_type, "index_of", string_index_of, int_type,
                            string_param, 1);
    builtin_register_method(vm, registry->builtins.str_type, str_type, "count", string_count, int_type,
                            string_param, 1);

    // Declared on the characters like every other reader, and returning the
    // owning string rather than the borrowed one: what it hands back is a fresh
    // allocation the caller frees.
    builtin_register_method(vm, registry->builtins.str_type, str_type, "to_owned", string_to_owned,
                            registry->builtins.string_type, NULL, 0);

    // Reached on the owning type, since that is what it yields: 'String::from'
    // hands back an allocation, where every method above reads a borrow.
    builtin_register_static(vm, string_type, "from", string_from, registry->builtins.string_type,
                            string_param, 1);

    // Growing belongs to the owner: what is written into is the block, and only
    // a 'String' has one. Each takes a pointer to the header for the reason
    // 'clone' does, and writes the grown header back through it.
    const Type *const char_param[] = {int_type};

    builtin_register_method(vm, string_type, ref_string, "push", string_push, NULL, char_param, 1);
    builtin_register_method(vm, string_type, ref_string, "append", string_append, NULL, string_param, 1);

    // The one method belonging to the owner rather than to the characters: what
    // it duplicates is the allocation, which only a 'String' has.
    //
    // Its receiver is a pointer to the header. A receiver by value would have to
    // copy it, which an owning string cannot do; and 'ref String' is the address
    // of a slot holding one, which is not what a 'ref str' is.
    builtin_register_method(vm, string_type, ref_string, "clone", string_clone,
                            registry->builtins.string_type, NULL, 0);
}
