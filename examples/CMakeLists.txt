# rt_periodic_loop shows the real-time foundation alone, so it links common-base only;
# unit_application shows the application patterns of common built on it.
add_executable(rt_periodic_loop rt_periodic_loop.cpp)
target_link_libraries(rt_periodic_loop PRIVATE common::base common::options)

add_executable(unit_application unit_application.cpp)
target_link_libraries(unit_application PRIVATE common::common common::options)

if(WIN32 AND BUILD_SHARED_LIBS)
    # Windows finds a DLL next to the executable (there is no rpath).
    add_custom_command(TARGET rt_periodic_loop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:common-base> $<TARGET_FILE_DIR:rt_periodic_loop>)
    add_custom_command(TARGET unit_application POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:common-base> $<TARGET_FILE:common>
                $<TARGET_FILE_DIR:unit_application>)
endif()
