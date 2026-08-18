cmake_minimum_required(VERSION 3.10)

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

add_library(gab
    src/arena.c
    src/diagnostics.c
    src/lexer.c
    src/parser.c
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

