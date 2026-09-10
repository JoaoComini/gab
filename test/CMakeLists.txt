# Every test binary is built the same way: it links the compiler as a library, asserts, and knows
# where the core the compiler wrote is. What differs between suites is what drives them.
function(gab_test_binary TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})

    # Every check in this suite is an assert, so a Release build defining NDEBUG
    # would compile the checks away and report a vacuous pass.
    target_compile_options(${TEST_NAME} PRIVATE -UNDEBUG
        -DGAB_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}"
        -DGAB_TEST_CORE_INTERFACE="${GAB_LIB_DIR}/core.gabi"
        -DGAB_TEST_CORE_SOURCE="${CMAKE_SOURCE_DIR}/lib/core.gab")

    target_link_libraries(${TEST_NAME} PRIVATE gab)
    target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # What a test resolves against is the interface the compiler wrote, so the core is built first.
    add_dependencies(${TEST_NAME} core)
endfunction()

# A test that compiles finds the core beside the compiler, which is the build root rather than here.
set(GAB_TEST_ENV "GABC_LIBDIR=${GAB_LIB_DIR}")

# The runtime a case links against carries the sanitizers this build was compiled with, which reaches
# the link through the compiler rather than around it.
if(GAB_SANITIZE)
    list(APPEND GAB_TEST_ENV "GABC_LINK_FLAGS=-fsanitize=${GAB_SANITIZE}")
endif()

# One binary per file, each run on its own.
set(
    TEST_SOURCES

    # Data structures and the arena everything allocates from.
    util/arena_test.c
    util/hash_map_test.c
    util/list_test.c
    util/string_pool_test.c

    # Text to tokens to a tree, and what a malformed one reports.
    syntax/diagnostics_test.c
    syntax/lexer_test.c
    syntax/parser_test.c

    # What the type registry holds: layout, interning, and generic instantiation.
    types/decl_id_test.c
    types/intrinsic_struct_test.c
    types/pointer_test.c
    types/raw_test.c
    types/scope_test.c
    types/type_generic_test.c
    types/type_layout_test.c

    # The IR the resolver lowers to, and the passes that rewrite it.
    mir/mir_drop_test.c
    mir/mir_fold_test.c
    mir/mir_liveness_test.c
    mir/mir_lower_test.c
    mir/mir_module_test.c
    mir/mir_print_test.c

    # The names and linkage a lowered body emits, which running one cannot show.
    llvm/symbols_test.c

    # What a compilation states to the next one, which only reading it back can show.
    driver/interface_test.c
    driver/module_test.c

    # The samples the README shows must still compile.
    readme_test.c
)

foreach(TEST_SOURCE ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)

    gab_test_binary(${TEST_NAME} ${TEST_SOURCE})

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

    set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT "${GAB_TEST_ENV}")
endforeach()

# A case in test/run is compiled, linked and run as a native binary, so what it checks is what the
# backend actually emitted rather than what a VM read.
gab_test_binary(run_test run_test.c)

target_compile_options(run_test PRIVATE
    -DGAB_TEST_COMPILER="$<TARGET_FILE:gabc>"
    -DGAB_TEST_TESTING_INTERFACE="${GAB_LIB_DIR}/testing.gabi"
    -DGAB_TEST_HARNESS="${CMAKE_CURRENT_SOURCE_DIR}/support/harness.c")

# The suite drives the compiler as a program, which is what links a case against the core.
add_dependencies(run_test gabc std)

# Globbed with CONFIGURE_DEPENDS so a new .gab file runs without being named here, which is
# what a forgotten registration would otherwise cost: a test that silently never runs.
file(GLOB RUN_CASES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/run/*.gab)

foreach(CASE ${RUN_CASES})
    get_filename_component(CASE_STEM ${CASE} NAME_WE)

    add_test(NAME run_${CASE_STEM} COMMAND run_test ${CMAKE_CURRENT_SOURCE_DIR}/run ${CASE_STEM})

    set_tests_properties(run_${CASE_STEM} PROPERTIES ENVIRONMENT "${GAB_TEST_ENV}")
endforeach()

# A rejection is stated by the program that provokes it: the case is source, and a trailing
# '// error' block names every diagnostic it must raise, at the position the compiler reports.
gab_test_binary(error_test error_test.c)

file(GLOB ERROR_CASES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/error/*.gab)

foreach(CASE ${ERROR_CASES})
    get_filename_component(CASE_STEM ${CASE} NAME_WE)

    add_test(NAME error_${CASE_STEM} COMMAND error_test ${CMAKE_CURRENT_SOURCE_DIR}/error ${CASE_STEM})

    set_tests_properties(error_${CASE_STEM} PROPERTIES ENVIRONMENT "${GAB_TEST_ENV}")
endforeach()
