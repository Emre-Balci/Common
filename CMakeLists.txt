cmake_minimum_required(VERSION 3.21)

project(common
    VERSION 0.1.0
    DESCRIPTION "Real-time C++17 foundation library"
    LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "common targets Linux only (use WSL on Windows)")
endif()

set(COMMON_IS_TOP_LEVEL ${PROJECT_IS_TOP_LEVEL})

option(COMMON_BUILD_TESTS "Build unit tests" ${COMMON_IS_TOP_LEVEL})
option(COMMON_BUILD_EXAMPLES "Build examples" ${COMMON_IS_TOP_LEVEL})
option(COMMON_WARNINGS_AS_ERRORS "Treat warnings as errors" ${COMMON_IS_TOP_LEVEL})
option(COMMON_ENABLE_EXCEPTIONS "Compile with exceptions and RTTI" OFF)
option(COMMON_ENABLE_CLANG_TIDY "Run clang-tidy during the build" OFF)
set(COMMON_SANITIZER "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(cmake/CommonOptions.cmake)

find_package(Threads REQUIRED)

# Sources mirror the header groups in include/common/<group>/.
add_library(common
    src/core/assert.cpp
    src/file/file.cpp
    src/numeric/float.cpp
    src/protocol/rate_limiter.cpp
    src/system/memory.cpp
    src/system/thread.cpp
    src/time/clock.cpp)
add_library(common::common ALIAS common)

target_include_directories(common
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(common PUBLIC cxx_std_17)
target_link_libraries(common
    PUBLIC Threads::Threads
    PRIVATE common::options)

if(COMMON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(COMMON_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
