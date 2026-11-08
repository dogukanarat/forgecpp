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

forge_add_test(forgecpp_pb_tests SOURCES test_pb.cpp)
forge_add_proto(forgecpp_pb_tests ROOT proto PROTOS forge_test/all_types.proto PYTHON "${CMAKE_CURRENT_BINARY_DIR}/pb_python")
# Golden data from Google's protobuf for Python (the generator needs it anyway).
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/pb_golden.jsonl"
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/pb_golden.py" "${CMAKE_CURRENT_BINARY_DIR}/pb_python"
            "${CMAKE_CURRENT_BINARY_DIR}/pb_golden.jsonl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/pb_golden.py" "${CMAKE_CURRENT_BINARY_DIR}/pb_python/forge_test/all_types_pb2.py"
    COMMENT "pb golden data"
    VERBATIM)
target_sources(forgecpp_pb_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/pb_golden.jsonl")
target_compile_definitions(forgecpp_pb_tests PRIVATE FORGE_PB_GOLDEN="${CMAKE_CURRENT_BINARY_DIR}/pb_golden.jsonl")

# A field without a bound is an error at build time, not a callback.
_forge_protoc(_test_protoc)
add_test(NAME forge_proto_refuses_unbounded
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/forge-proto.py" --protoc ${_test_protoc}
            --nanopb "${FORGECPP_NANOPB_DIR}" --out "${CMAKE_CURRENT_BINARY_DIR}/bad_proto"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/proto/bad" unbounded.proto)
set_tests_properties(forge_proto_refuses_unbounded PROPERTIES
    PASS_REGULAR_EXPRESSION "unbounded.text: give it a bound")

forge_add_test(forgecpp_zmq_tests SOURCES test_zmq.cpp)
forge_add_test(forgecpp_framework_tests SOURCES test_framework.cpp test_services.cpp)
# The services are internal (src/app/services.hpp).
target_include_directories(forgecpp_framework_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")

# The modules use only modules of their own level or lower (docs/CODING_STANDARDS.md, F-011).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME forgecpp_layers COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/check-layers.py")
endif()

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
