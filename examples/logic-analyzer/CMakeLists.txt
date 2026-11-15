# logic-analyzer: a four-channel logic analyzer on a Zynq-7000 (Arty Z7), web UI first.
#
# In-tree:     one of the forgecpp examples (install.sh --examples); not installed.
# Standalone:  cmake -S examples/logic-analyzer -B build -DCMAKE_PREFIX_PATH=<forgecpp prefix>
#              installs the daemon, its config and systemd unit (used by Yocto).
cmake_minimum_required(VERSION 3.16)
project(logic-analyzer VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
set(LA_STANDALONE OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(LA_STANDALONE ON)
endif()
if(NOT TARGET forgecpp::forgecpp)
    find_package(forgecpp 0.4 REQUIRED)
endif()

# Everything except main() lives in a static library so the tests can use it.
add_library(la_core STATIC
    src/capture.cpp src/decoders.cpp src/sim.cpp src/engine.cpp src/uio_source.cpp src/files.cpp src/app.cpp)
target_include_directories(la_core PUBLIC src)
target_link_libraries(la_core PUBLIC forgecpp::forgecpp)
target_compile_definitions(la_core PUBLIC LA_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(la_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(_install_mode)
if(NOT LA_STANDALONE)
    set(_install_mode NO_INSTALL)  # like the other examples
endif()
forge_add_app(logic-analyzer
    SOURCES src/main.cpp
    LIBS    la_core
    CONFIG  config/logic-analyzer.toml
    WEB     web
    DESCRIPTION "Four-channel logic analyzer (forgecpp)"
    RESTART on-failure MAX_RESTARTS 3 RESTART_WINDOW 600 RESTART_DELAY 2 WATCHDOG 30
    ${_install_mode})

# Run it from the build dir with its config next to it.
configure_file(config/logic-analyzer.toml "${CMAKE_CURRENT_BINARY_DIR}/logic-analyzer.toml" COPYONLY)

if(LA_STANDALONE)
    include(CTest)  # standalone build; in-tree, forgecpp enables testing
endif()
if(BUILD_TESTING OR FORGECPP_BUILD_TESTS)
    if(TARGET forgecpp::testing OR TARGET forgecpp_testing)
        forge_add_test(logic_analyzer_tests
            SOURCES tests/test_core.cpp tests/test_engine.cpp tests/test_app.cpp
            LIBS la_core)
    endif()
endif()
