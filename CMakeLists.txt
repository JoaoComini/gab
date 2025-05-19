cmake_minimum_required(VERSION 3.10)

project(gab C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_COMPILER clang)

add_library(gab
    src/ast.c
    src/lexer.c
    src/parser.c
    src/string_ref.c
    src/symbol_table.c
    src/vm/constant_pool.c
    src/vm/vm.c
    src/vm/codegen.c
    src/vm/scope.c
)

target_include_directories(gab PUBLIC src/)

enable_testing()
add_subdirectory(test)
