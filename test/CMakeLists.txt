set(
    TEST_SOURCES
    lexer_test.c
    parser_test.c
    util/hash_map_test.c
    vm/constant_pool_test.c
    vm/codegen_test.c
    vm/vm_test.c
)

function(add_gab_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    
    target_link_libraries(${TEST_NAME} PRIVATE gab)
    
    add_test(
        NAME ${TEST_NAME}
        COMMAND ${TEST_NAME}
    )
endfunction()

foreach(TEST_SOURCE ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
    add_gab_test(${TEST_NAME} ${TEST_SOURCE})
endforeach()
