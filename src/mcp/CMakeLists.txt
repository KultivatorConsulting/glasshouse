qt_add_library(glasshouse_mcp STATIC
    mcphttpserver.h     mcphttpserver.cpp
    mcpserver.h         mcpserver.cpp
)

target_link_libraries(glasshouse_mcp
    PUBLIC
        Qt6::Core
        Qt6::Network
    PRIVATE
        Qt6::Gui            # QImage / PNG encoding for screenshots
        Qt6::Multimedia     # QVideoSink / QVideoFrame for the frame grab
        glasshouse_core
        glasshouse_input
        glasshouse_video
)

target_include_directories(glasshouse_mcp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Probe driver: serves a synthetic session so the tool surface can be
# exercised with no PiKVMs attached. Same role as glasshouse-janusprobe.
qt_add_executable(glasshouse-mcpprobe mcpprobe_main.cpp)
target_link_libraries(glasshouse-mcpprobe
    PRIVATE
        glasshouse_mcp
        Qt6::Core
        Qt6::Network
)
set_target_properties(glasshouse-mcpprobe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
