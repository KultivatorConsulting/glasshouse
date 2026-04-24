qt_add_executable(glasshouse-cli main.cpp)

target_link_libraries(glasshouse-cli
    PRIVATE
        glasshouse_core
)

set_target_properties(glasshouse-cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
