qt_add_library(glasshouse_input STATIC
    coord_transform.h   coord_transform.cpp
    keymap.h            keymap.cpp
    input_router.h      input_router.cpp
)

target_link_libraries(glasshouse_input
    PUBLIC
        Qt6::Core
        Qt6::Gui
        glasshouse_core
)

target_include_directories(glasshouse_input
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
