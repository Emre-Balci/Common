add_executable(common_tests
    test_main.cpp
    test_clock.cpp
    test_containers.cpp
    test_float.cpp
    test_queues.cpp
    test_seqlock.cpp
    test_status.cpp
    test_thread.cpp)

target_link_libraries(common_tests PRIVATE common::common common::options)

add_test(NAME common_tests COMMAND common_tests)
