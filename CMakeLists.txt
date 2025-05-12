cmake_minimum_required(VERSION 3.10)

project(rule C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(rule STATIC src/lexer.c src/parser.c src/ast.c)

target_include_directories(rule PUBLIC src) 

enable_testing()
add_subdirectory(test)
