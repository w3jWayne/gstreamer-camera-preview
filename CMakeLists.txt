cmake_minimum_required(VERSION 3.16)

project(GStreamer-based-linux-camera-preview-app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)

add_executable(camera_app main.cpp)
target_link_libraries(camera_app PRIVATE PkgConfig::GSTREAMER)
