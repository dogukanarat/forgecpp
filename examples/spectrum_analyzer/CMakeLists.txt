# spectrum-analyzer: swept/realtime spectrum analyzer on the AD9364 RX path, web UI first.
#
# In-tree:     one of the forgecpp examples (install.sh --examples); not installed.
# Standalone:  cmake -S examples/spectrum_analyzer -B build -DCMAKE_PREFIX_PATH=<forgecpp prefix>
#              installs the daemon, its config and systemd unit (used by Yocto).
cmake_minimum_required(VERSION 3.16)
project(spectrum-analyzer VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
set(SA_STANDALONE OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(SA_STANDALONE ON)
endif()
if(NOT TARGET forgecpp::forgecpp)
    find_package(forgecpp 0.2 REQUIRED)
endif()
if(NOT TARGET forgecpp::zmq)
    message(FATAL_ERROR "spectrum-analyzer needs forgecpp built with ZeroMQ (forgecpp::zmq)")
endif()

# AD936x access (libiio backend + simulator) and DSP, shared with radio_controller.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common/ad936x ${CMAKE_CURRENT_BINARY_DIR}/ad936x)

# Everything except main() lives in a static library so the tests can use it.
add_library(sa_core STATIC src/spectrum.cpp src/engine.cpp src/analyzer.cpp)
target_include_directories(sa_core PUBLIC src)
target_link_libraries(sa_core PUBLIC ad936x forgecpp::forgecpp forgecpp::zmq)
target_compile_definitions(sa_core PUBLIC SA_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sa_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

configure_file(systemd/spectrum-analyzer.service.in
               "${CMAKE_CURRENT_BINARY_DIR}/spectrum-analyzer.service" @ONLY)

set(_install_mode)
if(NOT SA_STANDALONE)
    set(_install_mode NO_INSTALL)  # like the other examples
endif()
forge_add_app(spectrum-analyzer
    SOURCES src/main.cpp
    LIBS    sa_core
    CONFIG  config/spectrum-analyzer.toml
    WEB     web
    SERVICE "${CMAKE_CURRENT_BINARY_DIR}/spectrum-analyzer.service"
    ${_install_mode})

# Run it from the build dir with its config next to it.
configure_file(config/spectrum-analyzer.toml "${CMAKE_CURRENT_BINARY_DIR}/spectrum-analyzer.toml" COPYONLY)

if(SA_STANDALONE)
    include(CTest)  # standalone build; in-tree, forgecpp enables testing
endif()
if(BUILD_TESTING OR FORGECPP_BUILD_TESTS)
    if(TARGET forgecpp::testing OR TARGET forgecpp_testing)
        forge_add_test(spectrum_analyzer_tests SOURCES tests/test_spectrum.cpp tests/test_analyzer.cpp LIBS sa_core)
    endif()
endif()
