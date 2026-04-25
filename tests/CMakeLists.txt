qt_add_executable(test_config test_config.cpp)
target_link_libraries(test_config
    PRIVATE
        glasshouse_core
        Qt6::Test
)
add_test(NAME config COMMAND test_config)

qt_add_executable(test_coord_transform test_coord_transform.cpp)
target_link_libraries(test_coord_transform
    PRIVATE
        glasshouse_input
        Qt6::Test
)
add_test(NAME coord_transform COMMAND test_coord_transform)

qt_add_executable(test_keymap test_keymap.cpp)
target_link_libraries(test_keymap
    PRIVATE
        glasshouse_input
        Qt6::Test
        Qt6::Gui
)
add_test(NAME keymap COMMAND test_keymap)
