# qam-modem: Ethernet (UDP) <-> 16QAM bursts over an AD936x, through radio-controller.
#
# In-tree:     one of the forgecpp examples (install.sh --examples); not installed.
# Standalone:  cmake -S examples/qam-modem -B build -DCMAKE_PREFIX_PATH=<forgecpp prefix>
cmake_minimum_required(VERSION 3.16)
project(qam-modem VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
set(QAM_STANDALONE OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(QAM_STANDALONE ON)
endif()
if(NOT TARGET forgecpp::forgecpp)
    find_package(forgecpp 0.4 REQUIRED)
endif()

# radio messages (iq_block, tx_block), shared with radio-controller.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common/ad936x ${CMAKE_CURRENT_BINARY_DIR}/ad936x)

# The DSP alone (no forgecpp): framing, modulator, demodulator.
add_library(qam_dsp STATIC src/qam.cpp)
target_include_directories(qam_dsp PUBLIC src)
target_compile_features(qam_dsp PUBLIC cxx_std_20)

add_library(qam_core STATIC src/modem.cpp)
target_include_directories(qam_core PUBLIC src)
target_link_libraries(qam_core PUBLIC qam_dsp ad936x forgecpp::forgecpp)
target_compile_definitions(qam_core PUBLIC QAM_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(qam_dsp PRIVATE -Wall -Wextra -Wpedantic)
    # The filters vectorize only with reassociation (and on 32-bit ARM, NEON is not
    # IEEE exact), complex products without the NaN/inf recovery: about 2x less
    # CPU. The DSP has no NaN/inf/signed-zero logic.
    target_compile_options(qam_dsp PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:-O3 -funsafe-math-optimizations -fno-math-errno>
        # GCC: complex products without the NaN/inf recovery (clang 18 warns about the flag)
        $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU>>:-fcx-limited-range>
        # real time even in debug builds (CI runs the end-to-end tests in Debug)
        $<$<CONFIG:Debug>:-O2>)
    target_compile_options(qam_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(_install_mode)
if(NOT QAM_STANDALONE)
    set(_install_mode NO_INSTALL)  # like the other examples
endif()
forge_add_app(qam-modem
    SOURCES src/main.cpp
    LIBS    qam_core
    CONFIG  config/qam-modem.toml
    WEB     web
    DESCRIPTION "16QAM packet modem (forgecpp)"
    AFTER radio-controller.service
    RESTART on-failure MAX_RESTARTS 3 RESTART_WINDOW 600 RESTART_DELAY 2 WATCHDOG 30
    ${_install_mode})

configure_file(config/qam-modem.toml "${CMAKE_CURRENT_BINARY_DIR}/qam-modem.toml" COPYONLY)

if(QAM_STANDALONE)
    install(PROGRAMS clients/echo.py DESTINATION ${CMAKE_INSTALL_DATADIR}/qam-modem/clients)
    include(CTest)
endif()
if(BUILD_TESTING OR FORGECPP_BUILD_TESTS)
    if(TARGET forgecpp::testing OR TARGET forgecpp_testing)
        forge_add_test(qam_modem_tests SOURCES tests/test_qam.cpp tests/test_modem.cpp LIBS qam_core)
        if(TARGET radio_core)  # in-tree: end to end with the real radio-controller (simulator)
            target_link_libraries(qam_modem_tests PRIVATE radio_core)
            target_compile_definitions(qam_modem_tests PRIVATE QAM_WITH_RADIO_CONTROLLER)
        endif()
    endif()
endif()
