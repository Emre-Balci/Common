# Interface target carrying the project's compiler policy. Link it PRIVATE to apply the same
# warnings and code-generation rules to your own targets:
#   target_link_libraries(my_app PRIVATE common::common common::options)

add_library(common_options INTERFACE)
add_library(common::options ALIAS common_options)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # /W4 plus the off-by-default warnings closest to the GCC/Clang set below. Headers included with
    # <...> (the standard library, the Windows SDK) are external and are not checked.
    set(COMMON_MSVC_WARNINGS
        /W4
        /w14061  # enumerator not handled by a case label (-Wswitch-enum)
        /w14062  # enumerator not handled in a switch without default (-Wswitch)
        /w14242  # conversion with possible loss of data (-Wconversion)
        /w14254  # bit-field conversion with possible loss of data (-Wconversion)
        /w14263  # member function hides a virtual function (-Woverloaded-virtual)
        /w14265  # virtual functions but a non-virtual destructor (-Wnon-virtual-dtor)
        /w14287  # unsigned/negative constant mismatch
        /w14296  # comparison that is always true or false (-Wtype-limits)
        /w14365  # signed/unsigned conversion (-Wsign-conversion)
        /w14388  # signed/unsigned comparison (-Wsign-compare)
        /w14668  # undefined macro in #if (-Wundef)
        /w14826  # sign-extending conversion
        /w15038  # members initialised out of declaration order (-Wreorder)
        /w15262  # implicit fall-through (-Wimplicit-fallthrough)
        /wd4324  # the padding that alignas(kCacheLineSize) adds is intended
        /external:anglebrackets
        /external:W0)

    target_compile_options(common_options INTERFACE
        ${COMMON_MSVC_WARNINGS}
        $<$<BOOL:${COMMON_WARNINGS_AS_ERRORS}>:/WX>
        /permissive-
        /Zc:__cplusplus
        /utf-8
        # Floating point: /fp:precise never contracts a*b+c into FMA (MSVC 19.30, i.e. VS 2022, and
        # later), so results are identical across optimisation levels. /fp:fast is rejected in
        # core/config.hpp.
        /fp:precise
        $<$<NOT:$<BOOL:${COMMON_ENABLE_EXCEPTIONS}>>:/EHs-c->
        $<$<NOT:$<BOOL:${COMMON_ENABLE_EXCEPTIONS}>>:/GR->)
    # Without exceptions, the standard library must not use them either (it would warn, C4530).
    target_compile_definitions(common_options INTERFACE
        $<$<NOT:$<BOOL:${COMMON_ENABLE_EXCEPTIONS}>>:_HAS_EXCEPTIONS=0>)
else()
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
endif()

if(COMMON_SANITIZER)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # MSVC only has address; link.exe adds its runtime libraries by itself, and ignores
        # incremental linking for such modules (with a warning) unless it is turned off.
        target_compile_options(common_options INTERFACE /fsanitize=${COMMON_SANITIZER})
        target_link_options(common_options INTERFACE /INCREMENTAL:NO)
    else()
        target_compile_options(common_options INTERFACE -fsanitize=${COMMON_SANITIZER} -fno-omit-frame-pointer)
        target_link_options(common_options INTERFACE -fsanitize=${COMMON_SANITIZER})
    endif()
endif()

if(COMMON_ENABLE_CLANG_TIDY)
    find_program(COMMON_CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-18 REQUIRED)
    set(CMAKE_CXX_CLANG_TIDY ${COMMON_CLANG_TIDY_EXE})
endif()
