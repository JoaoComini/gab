set(TEST_SOURCES lexer_test.c parser_test.c)

function(add_rule_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    
    target_link_libraries(${TEST_NAME} PRIVATE rule)
    
    add_test(
        NAME ${TEST_NAME}
        COMMAND ${TEST_NAME}
    )
endfunction()

foreach(TEST_SOURCE ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
    add_rule_test(${TEST_NAME} ${TEST_SOURCE})
endforeach()
