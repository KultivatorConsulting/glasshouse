qt_add_executable(test_config test_config.cpp)

target_link_libraries(test_config
    PRIVATE
        glasshouse_core
        Qt6::Test
)

add_test(NAME config COMMAND test_config)
