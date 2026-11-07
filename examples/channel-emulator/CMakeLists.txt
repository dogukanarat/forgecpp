# channel-emulator: radio channel impairments in the TX path of radio-controller
# (between a TX client such as qam-modem and radio-controller's [zmq].tx).
#
# In-tree:     one of the forgecpp examples (install.sh --examples); not installed.
# Standalone:  cmake -S examples/channel-emulator -B build -DCMAKE_PREFIX_PATH=<forgecpp prefix>
cmake_minimum_required(VERSION 3.16)
project(channel-emulator VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
set(EMU_STANDALONE OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(EMU_STANDALONE ON)
endif()
if(NOT TARGET forgecpp::forgecpp)
    find_package(forgecpp 0.3 REQUIRED)
endif()

# radio.v1 messages (TxBlock) and the impairments (plain C++)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common/ad936x ${CMAKE_CURRENT_BINARY_DIR}/ad936x)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common/channel ${CMAKE_CURRENT_BINARY_DIR}/channel)

add_library(emulator_core STATIC src/emulator.cpp)
target_include_directories(emulator_core PUBLIC src)
target_link_libraries(emulator_core PUBLIC channel_dsp ad936x forgecpp::forgecpp)
target_compile_definitions(emulator_core PUBLIC EMULATOR_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(emulator_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(_install_mode)
if(NOT EMU_STANDALONE)
    set(_install_mode NO_INSTALL)  # like the other examples
endif()
forge_add_app(channel-emulator
    SOURCES src/main.cpp
    LIBS    emulator_core
    CONFIG  config/channel-emulator.toml
    DESCRIPTION "Radio channel emulator (forgecpp)"
    AFTER radio-controller.service
    RESTART on-failure MAX_RESTARTS 3 RESTART_WINDOW 600 RESTART_DELAY 2 WATCHDOG 30
    ${_install_mode})

configure_file(config/channel-emulator.toml "${CMAKE_CURRENT_BINARY_DIR}/channel-emulator.toml" COPYONLY)

if(EMU_STANDALONE)
    include(CTest)
endif()
if(BUILD_TESTING OR FORGECPP_BUILD_TESTS)
    if(TARGET forgecpp::testing OR TARGET forgecpp_testing)
        forge_add_test(channel_emulator_tests SOURCES tests/test_channel.cpp tests/test_emulator.cpp LIBS emulator_core)
        if(TARGET qam_core AND TARGET radio_core)  # in-tree: the modem through the emulator and the simulator
            target_link_libraries(channel_emulator_tests PRIVATE qam_core radio_core)
            target_compile_definitions(channel_emulator_tests PRIVATE EMU_WITH_MODEM)
        endif()
    endif()
endif()
