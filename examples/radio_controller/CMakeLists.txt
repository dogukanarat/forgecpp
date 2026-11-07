# radio-controller: AD9364 (AD936x) control and IQ streaming over ZeroMQ.
#
# In-tree:     one of the forgecpp examples (install.sh --examples); not installed.
# Standalone:  cmake -S examples/radio_controller -B build -DCMAKE_PREFIX_PATH=<forgecpp prefix>
#              installs the daemon, its config, systemd unit and clients (used by Yocto).
cmake_minimum_required(VERSION 3.16)
project(radio-controller VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
set(RADIO_STANDALONE OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(RADIO_STANDALONE ON)
endif()
if(NOT TARGET forgecpp::forgecpp)
    find_package(forgecpp 0.2 REQUIRED)
endif()

# AD936x access (libiio backend + simulator) and DSP, shared with spectrum_analyzer.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common/ad936x ${CMAKE_CURRENT_BINARY_DIR}/ad936x)

# Everything except main() lives in a static library so the tests can use it.
add_library(radio_core STATIC src/radio_controller.cpp)
target_include_directories(radio_core PUBLIC src)
target_link_libraries(radio_core PUBLIC ad936x forgecpp::forgecpp)
target_compile_definitions(radio_core PUBLIC RADIO_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(radio_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

configure_file(systemd/radio-controller.service.in
               "${CMAKE_CURRENT_BINARY_DIR}/radio-controller.service" @ONLY)

set(_install_mode)
if(NOT RADIO_STANDALONE)
    set(_install_mode NO_INSTALL)  # like the other examples
endif()
forge_add_app(radio-controller
    SOURCES src/main.cpp
    LIBS    radio_core
    CONFIG  config/radio-controller.toml
    WEB     web
    SERVICE "${CMAKE_CURRENT_BINARY_DIR}/radio-controller.service"
    ${_install_mode})

# Run it from the build dir with its config next to it.
configure_file(config/radio-controller.toml "${CMAKE_CURRENT_BINARY_DIR}/radio-controller.toml" COPYONLY)

if(RADIO_STANDALONE)
    install(PROGRAMS clients/rx_capture.py clients/tx_tone.py
            DESTINATION ${CMAKE_INSTALL_DATADIR}/radio-controller/clients)
    install(FILES clients/forge_msg.py DESTINATION ${CMAKE_INSTALL_DATADIR}/radio-controller/clients)
    # radio.v1 Python modules (forge.v1 ones come with forgecpp)
    install(DIRECTORY ${AD936X_PYTHON_DIR}/ DESTINATION ${CMAKE_INSTALL_DATADIR}/radio-controller/python
            FILES_MATCHING PATTERN "*.py")
endif()

if(RADIO_STANDALONE)
    include(CTest)  # standalone build; in-tree, forgecpp enables testing
endif()
if(BUILD_TESTING OR FORGECPP_BUILD_TESTS)
    if(TARGET forgecpp::testing OR TARGET forgecpp_testing)
        forge_add_test(radio_controller_tests SOURCES tests/test_radio.cpp LIBS radio_core)
        target_compile_definitions(radio_controller_tests PRIVATE
            RADIO_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    endif()
endif()
