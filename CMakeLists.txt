cmake_minimum_required(VERSION 4.3)
project(TouchpadGestures
    VERSION 1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVDEV REQUIRED libevdev)
pkg_check_modules(DBUS REQUIRED dbus-1)
pkg_check_modules(PULSE REQUIRED libpulse)

add_executable(touchpad-gestures main.cpp)

target_include_directories(touchpad-gestures PRIVATE
    ${LIBEVDEV_INCLUDE_DIRS}
    ${DBUS_INCLUDE_DIRS}
    ${PULSE_INCLUDE_DIRS}
)

target_link_libraries(touchpad-gestures PRIVATE
    ${LIBEVDEV_LIBRARIES}
    ${DBUS_LIBRARIES}
    ${PULSE_LIBRARIES}
)

target_compile_options(touchpad-gestures PRIVATE -O3 -Wall -Wextra -Wpedantic)


# ============================================================================
# АВТОМАТИЧЕСКАЯ УСТАНОВКА ЧЕРЕЗ CMAKE
# ============================================================================

# CMake ставит ТОЛЬКО исполняемый файл (для этого нужно sudo)
install(TARGETS touchpad-gestures DESTINATION /usr/local/bin)
