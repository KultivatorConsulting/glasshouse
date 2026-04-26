cmake_minimum_required(VERSION 3.22)
project(glasshouse LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compile-time version string for the About box and any other UI surface
# that wants to report what's running. The project() VERSION above is
# the source of truth.
add_compile_definitions(GLASSHOUSE_VERSION="${PROJECT_VERSION}")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.7 REQUIRED COMPONENTS
    Core Network WebSockets Test
    Gui Widgets Multimedia MultimediaWidgets
)
qt_standard_project_setup()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GStreamer REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0
    gstreamer-sdp-1.0
    gstreamer-webrtc-1.0
)

# Vendor yaml-cpp via FetchContent so the build has no system-package
# dependency beyond Qt. Build the library STATIC and suppress its tests/tools.
# yaml-cpp 0.8.0's top-level CMakeLists declares cmake_minimum_required < 3.5,
# which CMake 4.x rejects. Scoped override applied to its subtree only.
include(FetchContent)
set(YAML_BUILD_SHARED_LIBS       OFF CACHE BOOL   "" FORCE)
set(YAML_CPP_BUILD_TESTS         OFF CACHE BOOL   "" FORCE)
set(YAML_CPP_BUILD_TOOLS         OFF CACHE BOOL   "" FORCE)
set(YAML_CPP_BUILD_CONTRIB       OFF CACHE BOOL   "" FORCE)
set(YAML_CPP_INSTALL             OFF CACHE BOOL   "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        0.8.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(yaml-cpp)

enable_testing()

add_subdirectory(src/core)
add_subdirectory(src/input)
add_subdirectory(src/cli)
add_subdirectory(src/video)
add_subdirectory(src/viewer)
add_subdirectory(tests)
