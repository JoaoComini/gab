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

# The backend builds a module through the C API, which verifies what it is given
# rather than leaving a malformed instruction for a later tool to reject.
find_package(LLVM REQUIRED CONFIG)

find_library(MATH_LIBRARY m)

include(GNUInstallDirs)

# What a program links against sits beside the compiler in the build tree and in 'lib/gab' once
# installed, which is the pair of places 'gab_libdir' looks.
set(GAB_LIB_DIR ${CMAKE_BINARY_DIR})

# The compiler's own objects, linked into the program below and into each test that reaches past it.
# An object library rather than an archive: nothing outside this build links the compiler, and an
# installed 'libgab.a' would make these 45 headers an API this language does not offer.
add_library(gab OBJECT
    src/memory/allocator.c
    src/memory/arena.c
    src/string/string.c
    src/string/string_ref.c
    src/constant.c
    src/diagnostics.c

    src/syntax/lexer.c
    src/syntax/parser.c

    src/ast/ast.c
    src/ast/expr.c
    src/ast/stmt.c
    src/ast/type_expr.c
    src/ast/facts.c
    src/ast/pending.c
    src/ast/resolve.c

    src/object.c
    src/scope.c
    src/function_registry.c
    src/type/type.c
    src/type/type_registry.c

    src/mir/mir.c
    src/mir/mir_build.c
    src/mir/mir_lower.c
    src/mir/mir_module.c
    src/mir/mir_instantiate.c
    src/mir/mir_liveness.c
    src/mir/mir_state.c
    src/mir/mir_borrowck.c
    src/mir/mir_drop.c
    src/mir/mir_fold.c
    src/mir/mir_print.c

    src/llvm/llvm_emit.c
    src/llvm/llvm_symbol.c

    src/driver/compile.c
    src/driver/interface.c
    src/driver/link.c
)

target_include_directories(gab PUBLIC src/)
target_include_directories(gab SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})

target_link_libraries(gab PRIVATE LLVM)

if(MATH_LIBRARY)
    target_link_libraries(gab PRIVATE ${MATH_LIBRARY})
endif()

# The compiler as a program, which is what compiles the core below and what a test drives.
add_executable(gabc src/driver/main.c)

target_link_libraries(gabc PRIVATE gab)

# The core is compiled by the compiler, into a library beside it: what 'gabc' links a program against
# is the same archive an installation would ship, rather than something the test suite builds.
add_custom_command(
    OUTPUT ${GAB_LIB_DIR}/core.o
    COMMAND gabc --core -o ${GAB_LIB_DIR}/core.o ${CMAKE_SOURCE_DIR}/src/core/core.gab
    DEPENDS gabc ${CMAKE_SOURCE_DIR}/src/core/core.gab
    COMMENT "Compiling the core"
)

add_library(core STATIC ${GAB_LIB_DIR}/core.o)

set_target_properties(core PROPERTIES LINKER_LANGUAGE C ARCHIVE_OUTPUT_DIRECTORY ${GAB_LIB_DIR})

# 'bin' holds the compiler and 'lib/gab' what it links a program against, which is the layout
# 'gab_libdir' looks for before falling back to its own directory.
install(TARGETS gabc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(TARGETS core ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/gab)

install(FILES ${GAB_LIB_DIR}/core.gabi DESTINATION ${CMAKE_INSTALL_LIBDIR}/gab)

# The suite builds by default, since a working copy is where it is wanted; a distribution turns it off
# with -DBUILD_TESTING=OFF rather than building 25 binaries it will not run.
option(BUILD_TESTING "Build the test suite" ON)

if(BUILD_TESTING)
    enable_testing()

    add_subdirectory(test)
endif()
