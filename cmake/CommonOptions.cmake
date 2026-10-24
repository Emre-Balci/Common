# Interface target carrying the project's compiler policy. Link it PRIVATE to apply the same
# warnings and code-generation rules to your own targets:
#   target_link_libraries(my_app PRIVATE common::common common::options)

add_library(common_options INTERFACE)
add_library(common::options ALIAS common_options)

set(COMMON_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wold-style-cast
    -Wcast-align
    -Wcast-qual
    -Wdouble-promotion
    -Wfloat-equal
    -Wnull-dereference
    -Wundef
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wswitch-enum
    -Wimplicit-fallthrough
    -Wformat=2
    -Wmissing-declarations
    -Wvla)

set(COMMON_GCC_WARNINGS
    -Wuseless-cast
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op)

target_compile_options(common_options INTERFACE
    ${COMMON_WARNINGS}
    $<$<CXX_COMPILER_ID:GNU>:${COMMON_GCC_WARNINGS}>
    $<$<BOOL:${COMMON_WARNINGS_AS_ERRORS}>:-Werror>
    # Floating point: never contract a*b+c into FMA behind our back, so results are identical
    # across optimisation levels and compilers. -ffast-math is rejected in core/config.hpp.
    -ffp-contract=off
    $<$<NOT:$<BOOL:${COMMON_ENABLE_EXCEPTIONS}>>:-fno-exceptions>
    $<$<NOT:$<BOOL:${COMMON_ENABLE_EXCEPTIONS}>>:-fno-rtti>)

if(COMMON_SANITIZER)
    target_compile_options(common_options INTERFACE -fsanitize=${COMMON_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(common_options INTERFACE -fsanitize=${COMMON_SANITIZER})
endif()

if(COMMON_ENABLE_CLANG_TIDY)
    find_program(COMMON_CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-18 REQUIRED)
    set(CMAKE_CXX_CLANG_TIDY ${COMMON_CLANG_TIDY_EXE})
endif()
