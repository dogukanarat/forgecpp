# spectrum-analyzer: swept/realtime spectrum analyzer on the AD9364 RX path, web UI first.
#
# In-tree:     one of the forgecpp examples (install.sh --examples); not installed.
# Standalone:  cmake -S examples/spectrum-analyzer -B build -DCMAKE_PREFIX_PATH=<forgecpp prefix>
#              installs the daemon, its config and systemd unit (used by Yocto).
cmake_minimum_required(VERSION 3.16)
project(spectrum-analyzer VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
set(SA_STANDALONE OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(SA_STANDALONE ON)
endif()
if(NOT TARGET forgecpp::forgecpp)
    find_package(forgecpp 0.4 REQUIRED)
endif()

# AD936x access (libiio backend + simulator) and DSP, shared with radio-controller.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common/ad936x ${CMAKE_CURRENT_BINARY_DIR}/ad936x)

# Everything except main() lives in a static library so the tests can use it.
add_library(sa_core STATIC src/spectrum.cpp src/engine.cpp src/radio_tap.cpp src/analyzer.cpp)
target_include_directories(sa_core PUBLIC src)
target_link_libraries(sa_core PUBLIC ad936x forgecpp::forgecpp)
# sa.v1 messages (spectrum stream); Python modules for clients
set(SA_PYTHON_DIR "${CMAKE_CURRENT_BINARY_DIR}/python")
forge_add_proto(sa_core ROOT ${CMAKE_CURRENT_SOURCE_DIR}/proto PROTOS sa/v1/spectrum.proto PYTHON ${SA_PYTHON_DIR})
target_compile_definitions(sa_core PUBLIC SA_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sa_core PRIVATE -Wall -Wextra -Wpedantic)
endif()


set(_install_mode)
if(NOT SA_STANDALONE)
    set(_install_mode NO_INSTALL)  # like the other examples
endif()
forge_add_app(spectrum-analyzer
    SOURCES src/main.cpp
    LIBS    sa_core
    CONFIG  config/spectrum-analyzer.toml
    WEB     web
    DESCRIPTION "AD936x spectrum analyzer (forgecpp)"
    AFTER radio-controller.service
    RESTART on-failure MAX_RESTARTS 3 RESTART_WINDOW 600 RESTART_DELAY 2 WATCHDOG 30
    ${_install_mode})

if(SA_STANDALONE)
    install(FILES proto/sa/v1/spectrum.proto DESTINATION ${CMAKE_INSTALL_DATADIR}/spectrum-analyzer/proto/sa/v1)
    forge_install_python(DIRECTORY ${SA_PYTHON_DIR})  # sa.v1 in site-packages
    install(PROGRAMS clients/spectrum_watch.py DESTINATION ${CMAKE_INSTALL_DATADIR}/spectrum-analyzer/clients)
endif()

# Run it from the build dir with its config next to it.
configure_file(config/spectrum-analyzer.toml "${CMAKE_CURRENT_BINARY_DIR}/spectrum-analyzer.toml" COPYONLY)

if(SA_STANDALONE)
    include(CTest)  # standalone build; in-tree, forgecpp enables testing
endif()
if(BUILD_TESTING OR FORGECPP_BUILD_TESTS)
    if(TARGET forgecpp::testing OR TARGET forgecpp_testing)
        forge_add_test(spectrum_analyzer_tests
            SOURCES tests/test_spectrum.cpp tests/test_analyzer.cpp tests/test_radio_tap.cpp
            LIBS sa_core)
        if(TARGET radio_core)  # in-tree: also test against the real radio-controller
            target_link_libraries(spectrum_analyzer_tests PRIVATE radio_core)
            target_compile_definitions(spectrum_analyzer_tests PRIVATE SA_WITH_RADIO_CONTROLLER)
        endif()
    endif()
endif()
