qt_add_library(glasshouse_watch STATIC
    watcher.h           watcher.cpp
)

target_link_libraries(glasshouse_watch
    PUBLIC
        glasshouse_core
        Qt6::Core
        Qt6::Gui
        Qt6::Multimedia
)

target_include_directories(glasshouse_watch
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
