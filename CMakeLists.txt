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
    src/type.c
    src/type_registry.c
    src/string/string_ref.c
    src/string/string.c
    src/vm/constant_pool.c
    src/vm/vm.c
    src/vm/codegen.c
    src/vm/scope.c
)

find_library(MATH_LIBRARY m)
if(MATH_LIBRARY)
    target_link_libraries(gab PRIVATE ${MATH_LIBRARY})
endif()

target_include_directories(gab PUBLIC src/)

enable_testing()
add_subdirectory(test)

