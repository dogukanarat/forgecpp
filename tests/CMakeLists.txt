forge_add_test(forgecpp_tests
    SOURCES
        test_alarms.cpp
        test_application.cpp
        test_channel.cpp
        test_config.cpp
        test_console.cpp
        test_events.cpp
        test_http.cpp
        test_json.cpp
        test_log.cpp
        test_scheduler.cpp
        test_thread_pool.cpp
        test_web.cpp)

forge_add_test(forgecpp_zmq_tests SOURCES test_zmq.cpp)
forge_add_test(forgecpp_framework_tests SOURCES test_framework.cpp)
