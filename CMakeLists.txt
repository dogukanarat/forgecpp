cmake_minimum_required(VERSION 3.16)

project(forgecpp
    VERSION 0.4.0
    DESCRIPTION "Batteries-included C++20 framework for Linux daemons and services"
    HOMEPAGE_URL "https://github.com/dogukanarat/forgecpp"
    LANGUAGES C CXX)

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
option(FORGECPP_BUILD_HUB       "Build forge-hub (dashboard of the device's applications)" ${FORGECPP_TOP_LEVEL})
# ZeroMQ is a core dependency: every application has a command link and streams.
set(FORGECPP_WITH_ZMQ ON CACHE STRING "Deprecated: ZeroMQ is always used")
set(FORGECPP_SANITIZE "" CACHE STRING
    "Build with sanitizers: address, undefined, thread or a list (address;undefined)")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
endif()

if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
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
find_package(quill           CONFIG REQUIRED)
find_package(CLI11           CONFIG REQUIRED)
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
# Messages are nanopb structs (third_party/nanopb); the build only needs protoc
# and Python's protobuf module to generate them (cmake/ForgeHelpers.cmake).
# The bundled protobuf installs protoc with a CMake package.
find_package(protobuf CONFIG QUIET)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
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
    src/app/commands.cpp
    src/app/config.cpp
    src/app/observability.cpp
    src/app/http_frontend.cpp
    src/app/main_loop.cpp
    src/app/signals.cpp
    src/app/messaging.cpp
    src/app/remote_console.cpp
    src/app/state.cpp
    src/alarms.cpp
    src/audit.cpp
    src/auth.cpp
    src/base64.cpp
    src/c2.cpp
    src/can.cpp
    src/common.cpp
    src/config.cpp
    src/config_schema.cpp
    src/console.cpp
    src/control.cpp
    src/crash.cpp
    src/events.cpp
    src/gpio.cpp
    src/health.cpp
    src/hub.cpp
    src/http.cpp
    src/json.cpp
    src/log.cpp
    src/metrics.cpp
    src/pb.cpp
    src/rt.cpp
    src/scheduler.cpp
    src/serial.cpp
    src/status.cpp
    src/systemd.cpp
    src/telnet.cpp
    src/remote.cpp
    src/tmtc.cpp
    src/trace.cpp
    src/web.cpp
    src/worker.cpp
    src/zmq.cpp)
add_library(forgecpp::forgecpp ALIAS forgecpp)

# The framework's messages, generated into the library (forge/messages.forge.hpp, ...).
forge_add_proto(forgecpp ROOT proto PROTOS forge/messages.proto forge/hub.proto
    PYTHON "${PROJECT_BINARY_DIR}/python")

# Built-in dashboard (src/web/*) compiled into the library, served at /forge/.
forge_add_web(forgecpp DIR src/web PREFIX /forge/ SYMBOL forgecpp_builtin_assets)

target_compile_features(forgecpp PUBLIC cxx_std_20)
target_include_directories(forgecpp
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
        $<BUILD_INTERFACE:${FORGECPP_THREAD_POOL_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(forgecpp
    PUBLIC
        Threads::Threads
        quill::quill
        CLI11::CLI11
        concurrentqueue::concurrentqueue
        nlohmann_json::nlohmann_json
        tomlplusplus::tomlplusplus
        httplib::httplib
        ${FORGECPP_CPPZMQ_TARGET})

# linenoise (BSD-2-Clause, third_party/linenoise): line editing for forge::Console,
# compiled into the library with hidden symbols (no clash with an application's own copy).
set(FORGECPP_LINENOISE_DIR "${PROJECT_SOURCE_DIR}/third_party/linenoise")
target_sources(forgecpp PRIVATE "${FORGECPP_LINENOISE_DIR}/linenoise.c")
set_source_files_properties("${FORGECPP_LINENOISE_DIR}/linenoise.c" PROPERTIES COMPILE_OPTIONS "-w;-fvisibility=hidden")
target_include_directories(forgecpp PRIVATE "${FORGECPP_LINENOISE_DIR}")
# nanopb (zlib license, third_party/nanopb): the runtime of forge::pb messages.
set(FORGECPP_NANOPB_DIR "${PROJECT_SOURCE_DIR}/third_party/nanopb")
target_sources(forgecpp PRIVATE
    "${FORGECPP_NANOPB_DIR}/pb_common.c" "${FORGECPP_NANOPB_DIR}/pb_encode.c" "${FORGECPP_NANOPB_DIR}/pb_decode.c")
target_include_directories(forgecpp PUBLIC
    $<BUILD_INTERFACE:${FORGECPP_NANOPB_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/forgecpp/nanopb>)

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

# Optional components, each a shared library of its own, so that the core
# library needs neither (find_package(forgecpp) then forgecpp::sqlite, ...).
# Their libraries are submodules too (third_party/sqlite, third_party/lz4),
# built and installed by install.sh like the others:
#   forgecpp::sqlite   SQLite: forge::sqlite::Database, forge::Backlog (store and forward)
#   forgecpp::lz4      LZ4 frames: forge::lz4::compress / decompress
#   forgecpp::orm      sqlite_orm: tables as C++ structs, no SQL (forge/orm.hpp;
#                      header-only; sqlite_orm is AGPL-3.0 or bought MIT)
# Their libraries are private dependencies: an application links the
# component only, and needs the SQLite or LZ4 headers only to use them itself.
option(FORGECPP_WITH_SQLITE "Build forgecpp::sqlite (needs SQLite 3)" ON)
option(FORGECPP_WITH_LZ4 "Build forgecpp::lz4 (needs liblz4)" ON)
option(FORGECPP_WITH_SQLITE_ORM "Provide forgecpp::orm (needs sqlite_orm)" ON)
set(FORGECPP_COMPONENTS)
function(_forge_component target name)
    add_library(forgecpp::${name} ALIAS ${target})
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME forgecpp-${name}
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
        EXPORT_NAME ${name})
    target_link_libraries(${target} PUBLIC forgecpp)
    if(FORGECPP_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
if(FORGECPP_WITH_SQLITE)
    find_package(SQLite3)
    if(SQLite3_FOUND)
        add_library(forgecpp_sqlite SHARED src/sqlite.cpp src/backlog.cpp)
        _forge_component(forgecpp_sqlite sqlite)
        target_link_libraries(forgecpp_sqlite PRIVATE SQLite::SQLite3)
        list(APPEND FORGECPP_COMPONENTS sqlite)
    else()
        message(WARNING "SQLite 3 not found: forgecpp::sqlite is not built (-DFORGECPP_WITH_SQLITE=OFF to silence)")
    endif()
endif()
if(FORGECPP_WITH_LZ4)
    # LZ4's CMake package (third_party/lz4, install.sh), else its pkg-config
    # file (a build with make: distributions, OpenEmbedded).
    find_package(lz4 CONFIG QUIET)
    if(TARGET LZ4::lz4)
        set(_forge_lz4 LZ4::lz4)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(FORGE_LZ4 QUIET IMPORTED_TARGET liblz4)
        endif()
        if(FORGE_LZ4_FOUND)
            set(_forge_lz4 PkgConfig::FORGE_LZ4)
        endif()
    endif()
    if(_forge_lz4)
        add_library(forgecpp_lz4 SHARED src/lz4.cpp)
        _forge_component(forgecpp_lz4 lz4)
        target_link_libraries(forgecpp_lz4 PRIVATE ${_forge_lz4})
        list(APPEND FORGECPP_COMPONENTS lz4)
    else()
        message(WARNING "liblz4 not found: forgecpp::lz4 is not built (-DFORGECPP_WITH_LZ4=OFF to silence)")
    endif()
endif()
if(FORGECPP_WITH_SQLITE_ORM)
    # Header-only: nothing of it goes into a forgecpp library. An application
    # that links forgecpp::orm compiles sqlite_orm.h and links libsqlite3.
    find_package(SqliteOrm CONFIG QUIET)
    if(TARGET sqlite_orm::sqlite_orm)
        add_library(forgecpp_orm INTERFACE)
        add_library(forgecpp::orm ALIAS forgecpp_orm)
        set_target_properties(forgecpp_orm PROPERTIES EXPORT_NAME orm)
        target_link_libraries(forgecpp_orm INTERFACE forgecpp sqlite_orm::sqlite_orm)
        list(APPEND FORGECPP_COMPONENTS orm)
    else()
        message(WARNING "sqlite_orm not found: forgecpp::orm is not provided (-DFORGECPP_WITH_SQLITE_ORM=OFF to silence)")
    endif()
endif()

# Test helper target: forgecpp + doctest with a ready-made main(). An
# INTERFACE target that carries the main() as a source: nothing to install as
# a library, so find_package(forgecpp) works with the -dev package alone.
if(doctest_FOUND)
    add_library(forgecpp_testing INTERFACE)
    add_library(forgecpp::testing ALIAS forgecpp_testing)
    set_target_properties(forgecpp_testing PROPERTIES EXPORT_NAME testing)
    target_sources(forgecpp_testing INTERFACE
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/testing_main.cpp>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_DATADIR}/forgecpp/testing_main.cpp>")
    target_link_libraries(forgecpp_testing INTERFACE forgecpp doctest::doctest)
    install(FILES src/testing_main.cpp DESTINATION ${CMAKE_INSTALL_DATADIR}/forgecpp COMPONENT Development)
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
set(FORGECPP_HAS_ORM OFF)
if(TARGET forgecpp_orm)
    set(FORGECPP_HAS_ORM ON)
endif()
if(TARGET forgecpp_testing)
    list(APPEND _forge_targets forgecpp_testing)
    set(FORGECPP_HAS_TESTING ON)
endif()
list(APPEND _forge_targets forgecpp_zmq)
foreach(_c IN LISTS FORGECPP_COMPONENTS)
    list(APPEND _forge_targets forgecpp_${_c})
endforeach()

# Components: Runtime (the library, forgectl, the Python package) and
# Development (headers, CMake and pkg-config files, protos, the generators):
# cmake --install build --component Runtime, CPack, or a packager's split.
install(TARGETS ${_forge_targets}
    EXPORT forgecppTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Runtime NAMELINK_COMPONENT Development
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Development
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/forge DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT Development
        FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY "${PROJECT_BINARY_DIR}/forge_proto/forgecpp/forge" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT Development FILES_MATCHING PATTERN "*.pb.h" PATTERN "*.forge.hpp")
install(DIRECTORY proto/forge DESTINATION ${CMAKE_INSTALL_DATADIR}/forgecpp/proto COMPONENT Development
        PATTERN "API.json" EXCLUDE)
# Python package "forge": the generated forge/*_pb2.py, the stream framing and
# the c2 command client, in site-packages (from a build directory:
# PYTHONPATH=<build>/python).
foreach(py framing.py c2.py serial.py)
    configure_file(python/forge/${py} "${PROJECT_BINARY_DIR}/python/forge/${py}" COPYONLY)
endforeach()
# pip install <build>/python (a ground segment without the C++ toolchain).
configure_file(python/pyproject.toml.in "${PROJECT_BINARY_DIR}/python/pyproject.toml" @ONLY)
forge_install_python(DIRECTORY "${PROJECT_BINARY_DIR}/python")
install(TARGETS forgectl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime)
install(FILES "${PROJECT_BINARY_DIR}/include/forge/version.hpp"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/forge)
install(FILES "${FORGECPP_THREAD_POOL_DIR}/BS_thread_pool.hpp"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES "${FORGECPP_NANOPB_DIR}/pb.h" "${FORGECPP_NANOPB_DIR}/pb_common.h"
              "${FORGECPP_NANOPB_DIR}/pb_encode.h" "${FORGECPP_NANOPB_DIR}/pb_decode.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/forgecpp/nanopb)
# forge_add_proto() in applications: the nanopb generator and forge-proto.py.
install(DIRECTORY "${FORGECPP_NANOPB_DIR}/generator" DESTINATION ${CMAKE_INSTALL_DATADIR}/forgecpp/nanopb
        COMPONENT Development
        PATTERN "__pycache__" EXCLUDE PATTERN "*_pb2.py" EXCLUDE PATTERN "*.bat" EXCLUDE
        PATTERN "*py2*" EXCLUDE PATTERN "platformio_generator.py" EXCLUDE PATTERN "Makefile" EXCLUDE)
install(PROGRAMS tools/forge-proto.py DESTINATION ${CMAKE_INSTALL_DATADIR}/forgecpp COMPONENT Development)
# Native builds also install the protoc they used, so applications built
# against this install need no protobuf of their own.
if(FORGE_PROTOC AND NOT CMAKE_CROSSCOMPILING)
    # A program of this machine: libexec, not share. protobuf installs protoc
    # as a link to protoc-<version>: install the file, not the link (which
    # would point at nothing).
    get_filename_component(_forge_protoc_file "${FORGE_PROTOC}" REALPATH)
    install(PROGRAMS "${_forge_protoc_file}" DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/forgecpp RENAME protoc
            COMPONENT Development)
endif()

install(EXPORT forgecppTargets
    NAMESPACE forgecpp::
    DESTINATION ${FORGECPP_CMAKE_DIR})

configure_package_config_file(cmake/forgecppConfig.cmake.in
    "${PROJECT_BINARY_DIR}/forgecppConfig.cmake"
    INSTALL_DESTINATION ${FORGECPP_CMAKE_DIR}
    # Paths relative to the prefix, whatever libdir and datadir are (Debian
    # multiarch, a BSP's layout): the config computes them from its own location.
    PATH_VARS CMAKE_INSTALL_DATADIR CMAKE_INSTALL_LIBEXECDIR CMAKE_INSTALL_INCLUDEDIR)
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/forgecppConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion)
install(FILES
        "${PROJECT_BINARY_DIR}/forgecppConfig.cmake"
        "${PROJECT_BINARY_DIR}/forgecppConfigVersion.cmake"
        cmake/ForgeHelpers.cmake
        cmake/ForgeEmbed.cmake
        cmake/forge-app.service.in
    DESTINATION ${FORGECPP_CMAKE_DIR} COMPONENT Development)

# pkg-config for non-CMake consumers (plain Makefiles, autotools, meson).
set(FORGECPP_PC_REQUIRES "libzmq")
set(FORGECPP_PC_REQUIRES_PRIVATE "")
if(TARGET OpenSSL::SSL)
    string(APPEND FORGECPP_PC_REQUIRES_PRIVATE " openssl")
endif()
if(TARGET ZLIB::ZLIB)
    string(APPEND FORGECPP_PC_REQUIRES_PRIVATE " zlib")
endif()
# httplib's feature macros (SSL, zlib, ...) change its class layouts, so
# pkg-config users must compile with exactly the same definitions.
set(_httplib_defs "$<FILTER:$<TARGET_PROPERTY:httplib::httplib,INTERFACE_COMPILE_DEFINITIONS>,INCLUDE,.+>")
set(FORGECPP_PC_DEFINES "$<$<BOOL:${_httplib_defs}>:-D$<JOIN:${_httplib_defs}, -D>>")
configure_file(cmake/forgecpp.pc.in "${PROJECT_BINARY_DIR}/forgecpp.pc.gen" @ONLY)
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/forgecpp.pc" INPUT "${PROJECT_BINARY_DIR}/forgecpp.pc.gen")
install(FILES "${PROJECT_BINARY_DIR}/forgecpp.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig COMPONENT Development)

if(FORGECPP_INSTALL_TOOLS)
    install(PROGRAMS tools/forgecpp-new DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Development)
endif()

install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT Runtime)
install(FILES third_party/thread-pool/LICENSE.txt
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses RENAME BS_thread_pool-LICENSE.txt COMPONENT Development)

# API reference (Doxygen): cmake --build build --target forgecpp_docs -> build/docs/html
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    set(FORGE_DOCS_OUT "${PROJECT_BINARY_DIR}/docs")
    configure_file(docs/Doxyfile.in "${PROJECT_BINARY_DIR}/Doxyfile" @ONLY)
    add_custom_target(forgecpp_docs
        COMMAND ${DOXYGEN_EXECUTABLE} "${PROJECT_BINARY_DIR}/Doxyfile"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "API reference -> ${FORGE_DOCS_OUT}/html/index.html"
        VERBATIM)
endif()
