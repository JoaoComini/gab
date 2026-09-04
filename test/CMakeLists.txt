set(
    TEST_SOURCES
    arena_test.c
    diagnostics_test.c
    lexer_test.c
    parser_test.c
    scope_test.c
    string/string_pool_test.c
    object_test.c
    library_test.c
    type_layout_test.c
    type_generic_test.c
    util/hash_map_test.c
    util/list_test.c
    failed_compile_test.c
    vm/constant_pool_test.c
    vm/call_test.c
    vm/register_reuse_test.c
    vm/struct_value_test.c
    vm/codegen_test.c
    vm/loop_shape_test.c
    vm/chunk_test.c
    vm/encoding_test.c
    vm/pointer_test.c
    vm/method_test.c
    vm/impl_test.c
    vm/interface_test.c
    vm/generic_method_test.c
    vm/interface_bound_test.c
    vm/generic_struct_test.c
    vm/owned_func_test.c
    vm/heap_test.c
    vm/ownership_test.c
    vm/struct_lifetime_test.c
    vm/move_test.c
    vm/owning_param_test.c
    vm/field_ownership_test.c
    vm/uninitialized_test.c
    ast/cfg_test.c
    ast/clone_test.c
    vm/string_test.c
    vm/str_test.c
    vm/array_test.c
    std/string_method_test.c
    std/vec_test.c
    vm/neg_test.c
    vm/not_test.c
    vm/modulo_test.c
    vm/compound_assign_test.c
    vm/cast_test.c
    vm/compare_test.c
    vm/struct_lit_test.c
    vm/if_test.c
    vm/loop_test.c
    vm/arithmetic_test.c
    vm/stack_test.c
    vm/vm_test.c
    gab_api_test.c
    extern_test.c
    host_harness_test.c
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
