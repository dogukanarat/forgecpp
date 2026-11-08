forge_add_test(forgecpp_tests
    SOURCES
        test_alarms.cpp
        test_c2.cpp
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

# Wire compatibility with zephyrus (the microcontroller side of forgecpp):
# -DFORGE_ZEPHYRUS_DIR=<a zephyrus checkout> builds zephyrus's own C code on
# the host and compares it with forgecpp on the same input.
set(FORGE_ZEPHYRUS_DIR "" CACHE PATH "A zephyrus checkout, for the wire compatibility tests")
if(FORGE_ZEPHYRUS_DIR)
    set(_zmods "${FORGE_ZEPHYRUS_DIR}/modules")
    if(NOT EXISTS "${_zmods}/connectivity/link/c2/src/c2.c")
        message(FATAL_ERROR "FORGE_ZEPHYRUS_DIR: no zephyrus checkout at ${FORGE_ZEPHYRUS_DIR}")
    endif()
    # c2.c includes its private "c2.h" from its own folder: copy it next to the stub.
    set(_zc2 "${CMAKE_CURRENT_BINARY_DIR}/zephyrus_c2")
    configure_file("${_zmods}/connectivity/link/c2/src/c2.c" "${_zc2}/c2.c" COPYONLY)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/compat/zephyrus/private/c2.h" "${_zc2}/c2.h" COPYONLY)
    add_library(zephyrus_compat STATIC "${_zc2}/c2.c" "${_zmods}/core/common/src/common_error.c")
    target_include_directories(zephyrus_compat PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/compat/zephyrus/stubs"
        "${_zmods}/connectivity/link/c2/include"
        "${_zmods}/core/common/include")
    target_compile_definitions(zephyrus_compat PUBLIC
        CONFIG_ZEPHYRUS_C2_MODULE_ID=22 CONFIG_ZEPHYRUS_C2_LOG_LEVEL=0)
    set_target_properties(zephyrus_compat PROPERTIES C_STANDARD 11)
    forge_add_test(forgecpp_zephyrus_compat_tests SOURCES compat/zephyrus/test_zephyrus_compat.cpp)
    target_link_libraries(forgecpp_zephyrus_compat_tests PRIVATE zephyrus_compat)
endif()
