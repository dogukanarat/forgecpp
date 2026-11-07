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
set(FORGECPP_WITH_ZMQ AUTO CACHE STRING "Build forgecpp::zmq (ON, OFF or AUTO = if ZeroMQ is found)")
option(FORGECPP_BUILD_APPS      "Build the bundled daemons in apps/"    OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

# Optional: ZeroMQ messaging (forgecpp::zmq).
set(FORGECPP_HAS_ZMQ OFF)
if(NOT FORGECPP_WITH_ZMQ STREQUAL "OFF")
    if(FORGECPP_WITH_ZMQ STREQUAL "ON")
        find_package(cppzmq CONFIG REQUIRED)
    else()
        find_package(cppzmq CONFIG QUIET)
    endif()
    if(cppzmq_FOUND)
        set(FORGECPP_HAS_ZMQ ON)
        # cppzmq-static is the variant for a static libzmq build.
        if(TARGET libzmq)
            set(FORGECPP_CPPZMQ_TARGET cppzmq)
        else()
            set(FORGECPP_CPPZMQ_TARGET cppzmq-static)
        endif()
    endif()
endif()
message(STATUS "forgecpp: ZeroMQ support ${FORGECPP_HAS_ZMQ}")

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
    src/alarms.cpp
    src/config.cpp
    src/console.cpp
    src/events.cpp
    src/http.cpp
    src/json.cpp
    src/log.cpp
    src/scheduler.cpp
    src/systemd.cpp
    src/web.cpp)
add_library(forgecpp::forgecpp ALIAS forgecpp)

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
    PRIVATE
        replxx::replxx)

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

# ZeroMQ messaging: a separate library so daemons that do not use it do not
# link libzmq.
if(FORGECPP_HAS_ZMQ)
    add_library(forgecpp_zmq src/zmq.cpp)
    add_library(forgecpp::zmq ALIAS forgecpp_zmq)
    set_target_properties(forgecpp_zmq PROPERTIES
        EXPORT_NAME zmq
        VERSION   ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
    target_link_libraries(forgecpp_zmq PUBLIC forgecpp ${FORGECPP_CPPZMQ_TARGET})
    if(FORGECPP_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(forgecpp_zmq PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# ---------------------------------------------------------------------------
# Examples / tests / apps
# ---------------------------------------------------------------------------

# enable_testing() must come before the subdirectories that add tests (apps/).
if(FORGECPP_BUILD_TESTS AND NOT doctest_FOUND)
    message(WARNING "doctest not found: unit tests are disabled")
    set(FORGECPP_BUILD_TESTS OFF)
endif()
if(FORGECPP_BUILD_TESTS)
    enable_testing()
endif()

if(FORGECPP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(FORGECPP_BUILD_APPS)
    add_subdirectory(apps)
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
if(TARGET forgecpp_zmq)
    list(APPEND _forge_targets forgecpp_zmq)
endif()

install(TARGETS ${_forge_targets}
    EXPORT forgecppTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/forge DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")
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
    DESTINATION ${FORGECPP_CMAKE_DIR})

# pkg-config for non-CMake consumers (plain Makefiles, autotools, meson).
set(FORGECPP_PC_REQUIRES "spdlog")
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
