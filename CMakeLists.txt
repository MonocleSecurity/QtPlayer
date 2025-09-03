cmake_minimum_required(VERSION 3.21)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake CACHE STRING "Vcpkg toolchain file")

project(QtPlayer C CXX)

set(CMAKE_CXX_STANDARD 17)

find_package(FFMPEG REQUIRED)
find_package(Qt5Core REQUIRED)
find_package(Qt5Gui REQUIRED)
find_package(Qt5OpenGL REQUIRED)
find_package(Qt5Widgets REQUIRED)

add_executable(QtPlayer
main.cpp
mainwindow.h
mainwindow.cpp
mainwindow.ui
openglwidget.h
openglwidget.cpp)

include_directories(QtPlayer ${FFMPEG_INCLUDE_DIRS})

target_link_libraries(QtPlayer ${FFMPEG_LIBRARIES})
target_link_libraries(QtPlayer Qt5::Core)
target_link_libraries(QtPlayer Qt5::Gui)
target_link_libraries(QtPlayer Qt5::OpenGL)
target_link_libraries(QtPlayer Qt5::Widgets)
