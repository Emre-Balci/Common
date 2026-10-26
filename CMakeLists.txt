cmake_minimum_required(VERSION 3.21)

project(common
    VERSION 0.1.0
    DESCRIPTION "Real-time C++17 foundation library"
    LANGUAGES CXX)

if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Windows" OR
        CMAKE_SYSTEM_NAME STREQUAL "Android"))
    message(FATAL_ERROR "common targets Linux, Windows and Android only")
endif()
# On Android, a 64-bit ABI (arm64-v8a, x86_64): the 32-bit ones have a 32-bit time_t and off_t.
# core/config.hpp checks the API level.
if(ANDROID AND NOT (CMAKE_SIZEOF_VOID_P EQUAL 8))
    message(FATAL_ERROR "common needs a 64-bit Android ABI (arm64-v8a or x86_64), not ${CMAKE_ANDROID_ARCH_ABI}")
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

if(MSVC AND (NOT COMMON_ENABLE_EXCEPTIONS))
    # CMake's default flags turn on exceptions and RTTI (/EHsc /GR). common::options turns both off;
    # removing the defaults here, for this project's own targets, spares cl's warning D9025 about
    # overriding them.
    string(REGEX REPLACE "(^| )/(EHsc|GR)( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

include(cmake/CommonOptions.cmake)

find_package(Threads REQUIRED)

# Sources mirror the header groups in include/common/<group>/. A component whose implementation is
# mostly platform-specific has a <name>_posix.cpp and a <name>_win32.cpp next to its <name>.cpp.
add_library(common
    src/core/assert.cpp
    src/diagnostics/logger.cpp
    src/file/file.cpp
    src/file/mapped_file.cpp
    src/file/serial_port.cpp
    src/file/whole_file.cpp
    src/numeric/float.cpp
    src/protocol/rate_limiter.cpp
    src/system/memory.cpp
    src/system/thread.cpp
    src/time/clock.cpp)
if(WIN32)
    target_sources(common PRIVATE
        src/detail/overlapped_io.cpp
        src/file/serial_port_win32.cpp
        src/system/thread_win32.cpp)
else()
    target_sources(common PRIVATE
        src/file/serial_port_posix.cpp
        src/system/thread_posix.cpp)
endif()
add_library(common::common ALIAS common)
# A shared build (BUILD_SHARED_LIBS) exports every function on Windows too, as on Linux.
set_target_properties(common PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

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
