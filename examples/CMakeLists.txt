add_executable(rt_periodic_loop rt_periodic_loop.cpp)
target_link_libraries(rt_periodic_loop PRIVATE common::common common::options)

if(WIN32 AND BUILD_SHARED_LIBS)
    # Windows finds a DLL next to the executable (there is no rpath).
    add_custom_command(TARGET rt_periodic_loop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:common> $<TARGET_FILE_DIR:rt_periodic_loop>)
endif()
