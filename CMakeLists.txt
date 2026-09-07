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
    src/memory/allocator.c
    src/memory/arena.c
    src/diagnostics.c
    src/syntax/lexer.c
    src/syntax/parser.c
    src/object.c
    src/type/type.c
    src/type/type_registry.c
    src/function_registry.c
    src/scope.c
    src/ast/ast.c
    src/ast/facts.c
    src/ast/pending.c
    src/ast/resolve.c
    src/llvm/llvm_emit.c
    src/llvm/llvm_symbol.c
    src/mir/mir.c
    src/mir/mir_print.c
    src/mir/mir_build.c
    src/mir/mir_lower.c
    src/mir/mir_instantiate.c
    src/mir/mir_liveness.c
    src/mir/mir_state.c
    src/mir/mir_borrowck.c
    src/mir/mir_drop.c
    src/constant.c
    src/mir/mir_module.c
    src/mir/mir_fold.c
    src/ast/expr.c
    src/ast/type_expr.c
    src/ast/stmt.c
    src/string/string_ref.c
    src/string/string.c
    src/driver/compile.c
    src/driver/link.c
    src/driver/interface.c
)

# The compiler as a program: one source in, one native object out.
add_executable(gabc src/driver/main.c)

target_link_libraries(gabc PRIVATE gab)

# The core is compiled by the compiler, into a library beside it: what 'gabc' links a program against
# is the same archive an installation would ship, rather than something the test suite builds.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/core.o
    COMMAND gabc --core -o ${CMAKE_BINARY_DIR}/core.o ${CMAKE_SOURCE_DIR}/src/core/core.gab
    DEPENDS gabc ${CMAKE_SOURCE_DIR}/src/core/core.gab
    COMMENT "Compiling the core"
)

add_library(core STATIC ${CMAKE_BINARY_DIR}/core.o)

set_target_properties(core PROPERTIES LINKER_LANGUAGE C ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# What a compiled program links against: the runtime a native object calls into, built apart from the
# compiler so a binary carries neither the parser nor the emitter that produced it.
add_library(gab_runtime STATIC src/runtime/runtime.c)

set_target_properties(gab_runtime PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

target_include_directories(gab_runtime PUBLIC src)

find_library(MATH_LIBRARY m)
if(MATH_LIBRARY)
    target_link_libraries(gab PRIVATE ${MATH_LIBRARY})
endif()

# The backend builds a module through the C API, which verifies what it is given
# rather than leaving a malformed instruction for a later tool to reject.
find_package(LLVM REQUIRED CONFIG)

target_include_directories(gab SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(gab PRIVATE LLVM)

target_include_directories(gab PUBLIC src/)

enable_testing()
add_subdirectory(test)

