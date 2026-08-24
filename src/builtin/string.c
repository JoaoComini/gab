#include "builtin/builtin.h"

#include "object.h"
#include "type_registry.h"
#include "vm/args.h"
#include "vm/interp.h"
#include "vm/vm.h"

#include <stddef.h>
#include <string.h>

// Where 'needle' first occurs in 'haystack' at or after 'from', or -1. The
// empty needle occurs at the position asked for, which is what makes
// 'index_of("")' zero.
static int32_t string_find(GabStringValue haystack, GabStringValue needle, int32_t from) {
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
    GabStringValue string = args_string(args, 0);
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
    GabStringValue string = args_string(args, 0);
    GabStringValue prefix = args_string(args, 1);

    args_return_bool(args,
                     prefix.length <= string.length && memcmp(string.data, prefix.data, prefix.length) == 0);
}

// 's.ends_with(p)'. Whether the receiver's trailing characters spell 'p'. The
// length is checked first for the reason 'starts_with' gives, and here it also
// keeps the subtraction below from wrapping.
static void string_ends_with(Args *args) {
    GabStringValue string = args_string(args, 0);
    GabStringValue suffix = args_string(args, 1);

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
    GabStringValue string = args_string(args, 0);
    GabStringValue needle = args_string(args, 1);

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

void builtin_register_string(VM *vm) {
    TypeRegistry *registry = vm->env.global_scope.type_registry;

    Type *string_type = registry->builtins.string_type;
    Type *int_type = registry->builtins.int_type;
    Type *bool_type = registry->builtins.bool_type;

    Type *const int_param[] = {int_type};
    Type *const string_param[] = {string_type};

    builtin_register_method(vm, string_type, "len", string_len, int_type, NULL, 0);
    builtin_register_method(vm, string_type, "is_empty", string_is_empty, bool_type, NULL, 0);
    builtin_register_method(vm, string_type, "at", string_at, int_type, int_param, 1);
    builtin_register_method(vm, string_type, "starts_with", string_starts_with, bool_type, string_param, 1);
    builtin_register_method(vm, string_type, "ends_with", string_ends_with, bool_type, string_param, 1);
    builtin_register_method(vm, string_type, "contains", string_contains, bool_type, string_param, 1);
    builtin_register_method(vm, string_type, "index_of", string_index_of, int_type, string_param, 1);
    builtin_register_method(vm, string_type, "count", string_count, int_type, string_param, 1);
}
