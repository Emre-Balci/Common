# Test sources mirror the header groups in include/common/<group>/.
add_executable(common_tests
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

# test_framework.hpp lives here; tests in group subdirectories include it by name.
target_include_directories(common_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(common_tests PRIVATE common::common common::options)

add_test(NAME common_tests COMMAND common_tests)
