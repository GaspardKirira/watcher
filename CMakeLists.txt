cmake_minimum_required(VERSION 3.20)
project(watcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(watcher INTERFACE)
add_library(watcher::watcher ALIAS watcher)

target_include_directories(watcher INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

if (MSVC)
  target_compile_options(watcher INTERFACE /W4 /permissive-)
else()
  target_compile_options(watcher INTERFACE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
enable_testing()

add_executable(watcher_basic_test tests/test_basic.cpp)
target_link_libraries(watcher_basic_test PRIVATE watcher::watcher)
add_test(NAME watcher.basic COMMAND watcher_basic_test)
