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
    types/object_test.c
    types/pointer_test.c
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

    # The samples the README shows must still compile.
    readme_test.c
)

function(add_gab_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})

    # Every check in this suite is an assert, so a Release build defining NDEBUG
    # would compile the checks away and report a vacuous pass.
    target_compile_options(${TEST_NAME} PRIVATE -UNDEBUG)

    target_link_libraries(${TEST_NAME} PRIVATE gab)
    target_include_directories(${TEST_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    add_test(
        NAME ${TEST_NAME}
        COMMAND ${TEST_NAME}
    )
endfunction()

foreach(TEST_SOURCE ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
    add_gab_test(${TEST_NAME} ${TEST_SOURCE})
endforeach()

# A case in test/run is compiled, linked and run as a native binary, so what it checks is what the
# backend actually emitted rather than what a VM read.
add_executable(run_test run_test.c)

target_compile_options(run_test PRIVATE -UNDEBUG
    -DGAB_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}"
    -DGAB_TEST_RUNTIME="$<TARGET_FILE:gab_runtime>")

# The runtime a case links against carries the sanitizers this build was compiled with.
if(GAB_SANITIZE)
    target_compile_options(run_test PRIVATE -DGAB_TEST_LINK_FLAGS="-fsanitize=${GAB_SANITIZE}")
endif()

target_link_libraries(run_test PRIVATE gab)
target_include_directories(run_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Globbed with CONFIGURE_DEPENDS so a new .gab file runs without being named here, which is
# what a forgotten registration would otherwise cost: a test that silently never runs.
file(GLOB RUN_CASES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/run/*.gab)

foreach(CASE ${RUN_CASES})
    get_filename_component(CASE_STEM ${CASE} NAME_WE)

    # The prelude is included by every case rather than being one.
    if(NOT CASE_STEM STREQUAL "prelude")
        add_test(
            NAME run_${CASE_STEM}
            COMMAND run_test ${CMAKE_CURRENT_SOURCE_DIR}/run ${CASE_STEM}
        )
    endif()
endforeach()

# A rejection is stated by the program that provokes it: the case is source, and a trailing
# '// error' block names every diagnostic it must raise, at the position the compiler reports.
add_executable(error_test error_test.c)

target_compile_options(error_test PRIVATE -UNDEBUG)
target_link_libraries(error_test PRIVATE gab)
target_include_directories(error_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB ERROR_CASES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/error/*.gab)

foreach(CASE ${ERROR_CASES})
    get_filename_component(CASE_STEM ${CASE} NAME_WE)

    add_test(
        NAME error_${CASE_STEM}
        COMMAND error_test ${CMAKE_CURRENT_SOURCE_DIR}/error ${CASE_STEM}
    )
endforeach()
