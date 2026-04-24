qt_add_library(glasshouse_core STATIC
    logging.h           logging.cpp
    secrets.h           secrets.cpp
    config.h            config.cpp
    pikvmevents.h       pikvmevents.cpp
    pikvmclient.h       pikvmclient.cpp
)

target_link_libraries(glasshouse_core
    PUBLIC
        Qt6::Core
        Qt6::Network
        Qt6::WebSockets
    PRIVATE
        yaml-cpp::yaml-cpp
)

target_include_directories(glasshouse_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
