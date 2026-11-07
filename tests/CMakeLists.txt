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

# The source tree, for the check that defaults are written only in the schema.
target_compile_definitions(forgecpp_tests PRIVATE FORGE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

forge_add_test(forgecpp_zmq_tests SOURCES test_zmq.cpp)
forge_add_test(forgecpp_framework_tests SOURCES test_framework.cpp test_services.cpp)
# The services are internal (src/app/services.hpp).
target_include_directories(forgecpp_framework_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
