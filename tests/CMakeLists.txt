# Test sources mirror the header groups in include/common/<group>/. common_base_tests tests the
# groups of common-base and links only that library; common_tests tests the groups of common.
add_executable(common_base_tests
    test_main.cpp
    concurrency/test_broadcast_queue.cpp
    concurrency/test_queues.cpp
    concurrency/test_seqlock.cpp
    concurrency/test_triple_buffer.cpp
    containers/test_containers.cpp
    containers/test_intrusive_list.cpp
    containers/test_span.cpp
    containers/test_static_string.cpp
    control/test_control.cpp
    core/test_assert.cpp
    core/test_status.cpp
    diagnostics/test_latency_histogram.cpp
    diagnostics/test_logger.cpp
    diagnostics/test_watchdog.cpp
    file/test_file.cpp
    file/test_line_reader.cpp
    file/test_mapped_file.cpp
    file/test_serial_port.cpp
    file/test_whole_file.cpp
    numeric/test_decimal.cpp
    numeric/test_float.cpp
    numeric/test_integer.cpp
    protocol/test_backoff.cpp
    protocol/test_byte_order.cpp
    protocol/test_delimited_framer.cpp
    protocol/test_rate_limiter.cpp
    protocol/test_sequence_tracker.cpp
    protocol/test_text.cpp
    system/test_thread.cpp
    time/test_clock.cpp
    time/test_deadline.cpp
    time/test_timer_wheel.cpp)

# test_framework.hpp lives here; tests in group subdirectories include it by name. The internal
# headers of common-base (src/detail) are reachable too, for tests of platform helpers.
target_include_directories(common_base_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(common_base_tests PRIVATE common::base common::options)

add_test(NAME common_base_tests COMMAND common_base_tests)

add_executable(common_tests
    test_main.cpp
    app/test_application.cpp
    app/test_unit.cpp
    app/test_unit_manager.cpp
    event/test_signal.cpp
    functional/test_inplace_function.cpp
    object/test_object_type.cpp
    object/test_object_type_registry.cpp
    state/test_state_machine.cpp
    task/test_active_object.cpp
    task/test_task.cpp
    task/test_task_executor.cpp
    task/test_task_scheduler.cpp)

target_include_directories(common_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(common_tests PRIVATE common::common common::options)

add_test(NAME common_tests COMMAND common_tests)

# common-base must build without common. The groups are passed with commas, which survive the
# command line of the test unchanged.
string(REPLACE ";" "," COMMON_GROUPS_ARGUMENT "${COMMON_GROUPS}")
add_test(NAME common_base_layering
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DCOMMON_GROUPS=${COMMON_GROUPS_ARGUMENT}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_layering.cmake)

if(WIN32 AND BUILD_SHARED_LIBS)
    # Windows finds a DLL next to the executable (there is no rpath).
    add_custom_command(TARGET common_base_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:common-base> $<TARGET_FILE_DIR:common_base_tests>)
    add_custom_command(TARGET common_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:common-base> $<TARGET_FILE:common>
                $<TARGET_FILE_DIR:common_tests>)
endif()
