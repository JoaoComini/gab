cmake_minimum_required(VERSION 3.16)

project(gab C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_COMPILER clang)

# Warnings are errors. An API change that silently passed the wrong pointer
# type used to compile with only a warning, which is exactly the mistake a C
# compiler is able to catch for free.
#
# GAB_HASH_MAP and GAB_LIST expand to a full static API in every translation
# unit that includes them, so any unit using part of one warns about the rest.
# That is inherent to the macros rather than a defect, so it is the one warning
# turned off.
add_compile_options(-Wall -Wextra -Werror -Wno-unused-function)

# Sanitizers are a whole-program property: the library and the tests must agree
# on them, so this is a build option rather than a second set of targets.
# Configure a separate build directory to use it:
#
#   cmake -S . -B build-asan -DGAB_SANITIZE=address,undefined
#
# ASan finds the leaks and use-after-frees that manual refcounting invites;
# UBSan finds the overflow and alignment faults a VM decoding its own
# instructions can produce.
set(GAB_SANITIZE "" CACHE STRING "Sanitizers to build with, e.g. address,undefined")

if(GAB_SANITIZE)
    add_compile_options(-fsanitize=${GAB_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${GAB_SANITIZE})
endif()

add_library(gab
    src/arena.c
    src/diagnostics.c
    src/lexer.c
    src/parser.c
    src/refcounted.c
    src/type.c
    src/type_registry.c
    src/scope.c
    src/ast/ast.c
    src/ast/expr.c
    src/ast/stmt.c
    src/string/string_ref.c
    src/string/string.c
    src/vm/constant_pool.c
    src/vm/chunk.c
    src/vm/vm.c
    src/vm/codegen.c
    src/gab.c
)

find_library(MATH_LIBRARY m)
if(MATH_LIBRARY)
    target_link_libraries(gab PRIVATE ${MATH_LIBRARY})
endif()

target_include_directories(gab PUBLIC src/)

enable_testing()
add_subdirectory(test)

