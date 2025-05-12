cmake_minimum_required(VERSION 3.10)

project(rule C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_COMPILER clang)

enable_testing()
add_subdirectory(test)
