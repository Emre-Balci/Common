# Checks that common-base builds on its own: none of its files (headers, sources and tests) may
# include a header of a group of common. CTest runs it as
#   cmake -DSOURCE_DIR=<repository> -DCOMMON_GROUPS=<group>,<group>,... -P check_layering.cmake

if((NOT DEFINED SOURCE_DIR) OR (NOT DEFINED COMMON_GROUPS))
    message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<repository> -DCOMMON_GROUPS=<group>,... -P check_layering.cmake")
endif()

string(REPLACE "," "|" groups "${COMMON_GROUPS}")
file(GLOB_RECURSE candidates
    "${SOURCE_DIR}/include/common/*.hpp"
    "${SOURCE_DIR}/src/*.hpp"
    "${SOURCE_DIR}/src/*.cpp"
    "${SOURCE_DIR}/tests/*.hpp"
    "${SOURCE_DIR}/tests/*.cpp")

set(violations "")
foreach(file IN LISTS candidates)
    file(RELATIVE_PATH relative "${SOURCE_DIR}" "${file}")
    # The files of common: its groups, its tests and its umbrella header.
    if((relative MATCHES "^(include/common|src|tests)/(${groups})/") OR (relative STREQUAL "include/common/common.hpp"))
        continue()
    endif()
    file(STRINGS "${file}" lines REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<]common/(${groups})/")
    foreach(line IN LISTS lines)
        string(APPEND violations "\n  ${relative}: ${line}")
    endforeach()
endforeach()

if(violations)
    message(FATAL_ERROR "files of common-base include headers of common:${violations}")
endif()
list(LENGTH candidates count)
message(STATUS "checked ${count} files: common-base includes no header of common")
