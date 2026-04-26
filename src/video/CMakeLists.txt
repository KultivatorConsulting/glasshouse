qt_add_library(glasshouse_video STATIC
    videopipeline.h     videopipeline.cpp
    mjpegpipeline.h     mjpegpipeline.cpp
    videowindow.h       videowindow.cpp
    janusclient.h       janusclient.cpp
    specialkeysdialog.h specialkeysdialog.cpp
)

target_link_libraries(glasshouse_video
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Multimedia
        Qt6::MultimediaWidgets
        Qt6::Network
        Qt6::WebSockets
        glasshouse_input
    PRIVATE
        glasshouse_core
        PkgConfig::GStreamer
)

target_include_directories(glasshouse_video
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# gst-plugins-bad's WebRTC headers insist on this define to suppress the
# `#warning "unstable API"` spam. We knowingly depend on it — this is the
# only path stock PiKVM exposes for H.264.
target_compile_definitions(glasshouse_video PRIVATE GST_USE_UNSTABLE_API)

# Probe driver: signalling-only, no video decode. Lives here because it
# exercises JanusClient in isolation; will be deleted once the full
# webrtcbin path is wired into the viewer.
qt_add_executable(glasshouse-janusprobe janusprobe_main.cpp)
target_link_libraries(glasshouse-janusprobe
    PRIVATE
        glasshouse_core
        glasshouse_video
)
set_target_properties(glasshouse-janusprobe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
