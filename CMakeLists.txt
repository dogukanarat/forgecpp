cmake_minimum_required(VERSION 3.16)

project(forgecpp
    VERSION 0.2.0
    DESCRIPTION "Batteries-included C++17 framework for Linux daemons and services"
    HOMEPAGE_URL "https://github.com/dogukanarat/forgecpp"
    LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(FORGECPP_TOP_LEVEL ON)
else()
    set(FORGECPP_TOP_LEVEL OFF)
endif()

option(BUILD_SHARED_LIBS        "Build libforgecpp as a shared library" ON)
option(FORGECPP_BUILD_EXAMPLES  "Build the example applications"       ${FORGECPP_TOP_LEVEL})
option(FORGECPP_BUILD_TESTS     "Build the unit tests (needs doctest)"  ${FORGECPP_TOP_LEVEL})
option(FORGECPP_INSTALL_TOOLS   "Install the forgecpp-new scaffolder"   ON)
option(FORGECPP_WARNINGS        "Enable extra compiler warnings"        ${FORGECPP_TOP_LEVEL})
option(FORGECPP_BUILD_HUB       "Build forge-hub (device dashboard and supervisor)" ${FORGECPP_TOP_LEVEL})
# ZeroMQ and protobuf are core dependencies: every application speaks forge.v1.
set(FORGECPP_WITH_ZMQ ON CACHE STRING "Deprecated: ZeroMQ is always used")
set(FORGECPP_SANITIZE "" CACHE STRING
    "Build with sanitizers: address, undefined, thread or a list (address;undefined)")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Sanitizers apply to everything built here (library, tools, tests, examples).
# The dependencies in the prefix are not instrumented: for ThreadSanitizer
# build them with it too (install.sh --sanitize thread) to avoid false reports.
if(FORGECPP_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "FORGECPP_SANITIZE is for GCC and Clang")
    endif()
    if("thread" IN_LIST FORGECPP_SANITIZE AND "address" IN_LIST FORGECPP_SANITIZE)
        message(FATAL_ERROR "FORGECPP_SANITIZE: thread and address cannot be combined")
    endif()
    string(REPLACE ";" "," _forge_san "${FORGECPP_SANITIZE}")
    add_compile_options(-fsanitize=${_forge_san} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${_forge_san})
    if("undefined" IN_LIST FORGECPP_SANITIZE)
        add_compile_options(-fno-sanitize-recover=undefined)
    endif()
    message(STATUS "forgecpp: sanitizers ${_forge_san}")
endif()

# ---------------------------------------------------------------------------
# Dependencies. They are built from the git submodules in third_party/ and
# installed into the same prefix by install.sh, so everything below is found
# with plain find_package().
# ---------------------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(spdlog          CONFIG REQUIRED)
find_package(CLI11           CONFIG REQUIRED)
find_package(replxx          CONFIG REQUIRED)
find_package(concurrentqueue CONFIG REQUIRED)
find_package(nlohmann_json   CONFIG REQUIRED)
find_package(tomlplusplus    CONFIG REQUIRED)
find_package(httplib         CONFIG REQUIRED)
find_package(doctest         CONFIG)
find_package(cppzmq          CONFIG REQUIRED)
# cppzmq-static is the variant for a static libzmq build.
if(TARGET libzmq)
    set(FORGECPP_CPPZMQ_TARGET cppzmq)
else()
    set(FORGECPP_CPPZMQ_TARGET cppzmq-static)
endif()
set(FORGECPP_HAS_ZMQ ON)
# protobuf: the bundled one installs a CMake package; distributions ship FindProtobuf.
find_package(protobuf CONFIG QUIET)
if(NOT protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()
if(NOT TARGET protobuf::libprotobuf)
    message(FATAL_ERROR "protobuf found but protobuf::libprotobuf is missing")
endif()
set(FORGECPP_PROTO_DIR "${PROJECT_SOURCE_DIR}/proto")

# BS::thread_pool is a single header without a CMake package; we ship it
# ourselves next to the forgecpp headers.
set(FORGECPP_THREAD_POOL_DIR "${PROJECT_SOURCE_DIR}/third_party/thread-pool/include"
    CACHE PATH "Directory containing BS_thread_pool.hpp")
if(NOT EXISTS "${FORGECPP_THREAD_POOL_DIR}/BS_thread_pool.hpp")
    message(FATAL_ERROR
        "BS_thread_pool.hpp not found in ${FORGECPP_THREAD_POOL_DIR}. "
        "Run 'git submodule update --init --recursive' or use ./install.sh.")
endif()

# ---------------------------------------------------------------------------
# libforgecpp
# ---------------------------------------------------------------------------
configure_file(include/forge/version.hpp.in
               "${PROJECT_BINARY_DIR}/include/forge/version.hpp" @ONLY)

include("${PROJECT_SOURCE_DIR}/cmake/ForgeHelpers.cmake")

add_library(forgecpp
    src/application.cpp
    src/app/http_frontend.cpp
    src/app/messaging.cpp
    src/app/remote_console.cpp
    src/app/state.cpp
    src/alarms.cpp
    src/audit.cpp
    src/auth.cpp
    src/config.cpp
    src/config_schema.cpp
    src/console.cpp
    src/control.cpp
    src/crash.cpp
    src/events.cpp
    src/health.cpp
    src/hub.cpp
    src/http.cpp
    src/json.cpp
    src/log.cpp
    src/metrics.cpp
    src/proto.cpp
    src/rt.cpp
    src/scheduler.cpp
    src/systemd.cpp
    src/telnet.cpp
    src/trace.cpp
    src/web.cpp
    src/zmq.cpp)
add_library(forgecpp::forgecpp ALIAS forgecpp)

# forge.v1 messages, generated into the library (forge/v1/*.pb.h).
forge_add_proto(forgecpp ROOT proto PROTOS
    forge/v1/common.proto forge/v1/observability.proto forge/v1/control.proto forge/v1/hub.proto
    PYTHON "${PROJECT_BINARY_DIR}/python")

# Built-in dashboard (src/web/*) compiled into the library, served at /forge/.
forge_add_web(forgecpp DIR src/web PREFIX /forge/ SYMBOL forgecpp_builtin_assets)

target_compile_features(forgecpp PUBLIC cxx_std_17)
target_include_directories(forgecpp
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
        $<BUILD_INTERFACE:${FORGECPP_THREAD_POOL_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(forgecpp
    PUBLIC
        Threads::Threads
        spdlog::spdlog
        CLI11::CLI11
        concurrentqueue::concurrentqueue
        nlohmann_json::nlohmann_json
        tomlplusplus::tomlplusplus
        httplib::httplib
        ${FORGECPP_CPPZMQ_TARGET}
        protobuf::libprotobuf
    PRIVATE
        replxx::replxx)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # backtrace() lives in libc on glibc; musl needs libexecinfo (optional).
    find_library(FORGE_EXECINFO execinfo)
    if(FORGE_EXECINFO)
        target_link_libraries(forgecpp PRIVATE ${FORGE_EXECINFO})
    endif()
endif()

set_target_properties(forgecpp PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    EXPORT_NAME forgecpp)

if(FORGECPP_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(forgecpp PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Test helper target: forgecpp + doctest with a ready-made main().
if(doctest_FOUND)
    add_library(forgecpp_testing STATIC src/testing_main.cpp)
    add_library(forgecpp::testing ALIAS forgecpp_testing)
    set_target_properties(forgecpp_testing PROPERTIES EXPORT_NAME testing)
    target_link_libraries(forgecpp_testing PUBLIC forgecpp doctest::doctest)
endif()

# forgecpp::zmq: kept for applications written when ZeroMQ was optional.
add_library(forgecpp_zmq INTERFACE)
add_library(forgecpp::zmq ALIAS forgecpp_zmq)
set_target_properties(forgecpp_zmq PROPERTIES EXPORT_NAME zmq)
target_link_libraries(forgecpp_zmq INTERFACE forgecpp)

# forgectl: command line client for every forgecpp application.
add_executable(forgectl tools/forgectl.cpp)
target_link_libraries(forgectl PRIVATE forgecpp)
if(FORGECPP_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(forgectl PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Examples / tests / apps
# ---------------------------------------------------------------------------

# enable_testing() must come before the subdirectories that add tests (examples/).
if(FORGECPP_BUILD_TESTS AND NOT doctest_FOUND)
    message(WARNING "doctest not found: unit tests are disabled")
    set(FORGECPP_BUILD_TESTS OFF)
endif()
if(FORGECPP_BUILD_TESTS)
    enable_testing()
endif()

if(FORGECPP_BUILD_HUB)
    add_subdirectory(hub)
endif()

if(FORGECPP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(FORGECPP_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / package config
# ---------------------------------------------------------------------------
set(FORGECPP_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/forgecpp")

set(_forge_targets forgecpp)
set(FORGECPP_HAS_TESTING OFF)
if(TARGET forgecpp_testing)
    list(APPEND _forge_targets forgecpp_testing)
    set(FORGECPP_HAS_TESTING ON)
endif()
list(APPEND _forge_targets forgecpp_zmq)

install(TARGETS ${_forge_targets}
    EXPORT forgecppTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/forge DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY "${PROJECT_BINARY_DIR}/forge_proto/forgecpp/forge" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.pb.h")
install(DIRECTORY proto/forge DESTINATION ${CMAKE_INSTALL_DATADIR}/forgecpp/proto)
install(DIRECTORY "${PROJECT_BINARY_DIR}/python/" DESTINATION ${CMAKE_INSTALL_DATADIR}/forgecpp/python
        FILES_MATCHING PATTERN "*.py")
install(TARGETS forgectl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "${PROJECT_BINARY_DIR}/include/forge/version.hpp"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/forge)
install(FILES "${FORGECPP_THREAD_POOL_DIR}/BS_thread_pool.hpp"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT forgecppTargets
    NAMESPACE forgecpp::
    DESTINATION ${FORGECPP_CMAKE_DIR})

configure_package_config_file(cmake/forgecppConfig.cmake.in
    "${PROJECT_BINARY_DIR}/forgecppConfig.cmake"
    INSTALL_DESTINATION ${FORGECPP_CMAKE_DIR})
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/forgecppConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion)
install(FILES
        "${PROJECT_BINARY_DIR}/forgecppConfig.cmake"
        "${PROJECT_BINARY_DIR}/forgecppConfigVersion.cmake"
        cmake/ForgeHelpers.cmake
        cmake/ForgeEmbed.cmake
        cmake/forge-app.service.in
    DESTINATION ${FORGECPP_CMAKE_DIR})

# pkg-config for non-CMake consumers (plain Makefiles, autotools, meson).
set(FORGECPP_PC_REQUIRES "spdlog protobuf libzmq")
if(TARGET OpenSSL::SSL)
    string(APPEND FORGECPP_PC_REQUIRES " openssl")
endif()
if(TARGET ZLIB::ZLIB)
    string(APPEND FORGECPP_PC_REQUIRES " zlib")
endif()
# httplib's feature macros (SSL, zlib, ...) change its class layouts, so
# pkg-config users must compile with exactly the same definitions.
set(_httplib_defs "$<FILTER:$<TARGET_PROPERTY:httplib::httplib,INTERFACE_COMPILE_DEFINITIONS>,INCLUDE,.+>")
set(FORGECPP_PC_DEFINES "$<$<BOOL:${_httplib_defs}>:-D$<JOIN:${_httplib_defs}, -D>>")
configure_file(cmake/forgecpp.pc.in "${PROJECT_BINARY_DIR}/forgecpp.pc.gen" @ONLY)
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/forgecpp.pc" INPUT "${PROJECT_BINARY_DIR}/forgecpp.pc.gen")
install(FILES "${PROJECT_BINARY_DIR}/forgecpp.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(FORGECPP_INSTALL_TOOLS)
    install(PROGRAMS tools/forgecpp-new DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES third_party/thread-pool/LICENSE.txt
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses RENAME BS_thread_pool-LICENSE.txt)
