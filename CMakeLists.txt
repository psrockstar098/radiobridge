cmake_minimum_required(VERSION 3.16)
project(radiobridge VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(RB_BUILD_TESTS "Build the CTest suite" ON)
option(RB_BUILD_EXAMPLES "Build the C examples" ON)
option(RB_BUILD_SHARED "Build a shared library instead of static" OFF)

find_package(Threads REQUIRED)

if(RB_BUILD_SHARED)
  add_library(radiobridge SHARED
    src/core/rb_core.c
    src/bluetooth/rb_mock_ble.c
    src/wifi/rb_mock_wifi.c
    src/platform/linux/rb_linux.c
    src/platform/stubs/rb_stubs.c)
else()
  add_library(radiobridge STATIC
    src/core/rb_core.c
    src/bluetooth/rb_mock_ble.c
    src/wifi/rb_mock_wifi.c
    src/platform/linux/rb_linux.c
    src/platform/stubs/rb_stubs.c)
endif()

target_include_directories(radiobridge
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(radiobridge PRIVATE Threads::Threads)
target_compile_options(radiobridge PRIVATE -Wall -Wextra)

# Tests link the same library; they also include internal headers.
if(RB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(RB_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
