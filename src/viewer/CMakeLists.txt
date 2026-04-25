qt_add_executable(glasshouse-viewer main.cpp)

target_link_libraries(glasshouse-viewer
    PRIVATE
        glasshouse_core
        glasshouse_input
        glasshouse_video
        Qt6::Widgets
)

set_target_properties(glasshouse-viewer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
