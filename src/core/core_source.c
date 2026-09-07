#include "core/core_source.h"

/* 'str' reads as a slice of bytes, so what a slice declares precedes it. */
static const char CORE_SRC[] = "interface Index<T> {\n"
                               "    func index(self: &Self, at: i32): &T;\n"
                               "}\n"
                               "impl<T> slice<T> {\n"
                               "    intrinsic func len(self: &slice<T>): i32;\n"
                               "}\n"
                               "impl<T> slice<T> as Index<T> {\n"
                               "    intrinsic func index(self: &slice<T>, at: i32): &T;\n"
                               "}\n"
                               "impl<T, N: i32> array<T, N> {\n"
                               "    intrinsic func len(self: &Self): i32;\n"
                               "}\n"
                               "impl<T, N: i32> array<T, N> as Index<T> {\n"
                               "    intrinsic func index(self: &Self, at: i32): &T;\n"
                               "}\n"
                               "impl str {\n"
                               "    intrinsic func as_bytes(self: &str): &slice<u8>;\n"
                               "    func len(self: &str): i32 { return self.as_bytes().len(); }\n"
                               "}\n";

const char *core_source(void) { return CORE_SRC; }
