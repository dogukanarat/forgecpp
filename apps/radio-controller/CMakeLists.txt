# radio-controller: AD9364 (AD936x) control and IQ streaming over ZeroMQ.
#
# Standalone:  cmake -S apps/radio-controller -B build -DCMAKE_PREFIX_PATH=<forgecpp prefix>
# In-tree:     forgecpp with -DFORGECPP_BUILD_APPS=ON (install.sh --apps)
cmake_minimum_required(VERSION 3.16)
project(radio-controller VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
if(NOT TARGET forgecpp::forgecpp)
    find_package(forgecpp 0.2 REQUIRED)
endif()
if(NOT TARGET forgecpp::zmq)
    message(FATAL_ERROR "radio-controller needs forgecpp built with ZeroMQ (forgecpp::zmq)")
endif()

# libiio (0.x API). Without it only the simulator backend is available.
option(RADIO_WITH_IIO "Build the libiio backend for AD936x hardware" ON)
set(RADIO_HAS_IIO OFF)
if(RADIO_WITH_IIO)
    find_path(IIO_INCLUDE_DIR iio.h)
    find_library(IIO_LIBRARY NAMES iio libiio)
    if(IIO_INCLUDE_DIR AND IIO_LIBRARY)
        set(RADIO_HAS_IIO ON)
    else()
        message(WARNING "radio-controller: libiio not found - building with the simulator backend only")
    endif()
endif()
message(STATUS "radio-controller: libiio backend ${RADIO_HAS_IIO}")

# Everything except main() lives in a static library so the tests can use it.
add_library(radio_core STATIC
    src/dsp.cpp
    src/radio_controller.cpp
    src/sim_backend.cpp
    src/backend.cpp)
target_include_directories(radio_core PUBLIC src)
target_compile_features(radio_core PUBLIC cxx_std_17)
target_link_libraries(radio_core PUBLIC forgecpp::forgecpp forgecpp::zmq)
if(RADIO_HAS_IIO)
    target_sources(radio_core PRIVATE src/iio_backend.cpp)
    target_include_directories(radio_core PRIVATE ${IIO_INCLUDE_DIR})
    target_link_libraries(radio_core PUBLIC ${IIO_LIBRARY})
    if(IIO_LIBRARY MATCHES "\\.a$")
        # A static libiio does not carry its own dependencies.
        find_package(LibXml2 REQUIRED)
        find_package(Threads REQUIRED)
        target_link_libraries(radio_core PUBLIC LibXml2::LibXml2 Threads::Threads)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
            if(LIBUSB_FOUND)
                target_link_libraries(radio_core PUBLIC PkgConfig::LIBUSB)
            endif()
        endif()
    endif()
    target_compile_definitions(radio_core PUBLIC RADIO_HAS_IIO=1)
endif()
target_compile_definitions(radio_core PUBLIC RADIO_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(radio_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

configure_file(systemd/radio-controller.service.in
               "${CMAKE_CURRENT_BINARY_DIR}/radio-controller.service" @ONLY)

forge_add_app(radio-controller
    SOURCES src/main.cpp
    LIBS    radio_core
    CONFIG  config/radio-controller.toml
    WEB     web
    SERVICE "${CMAKE_CURRENT_BINARY_DIR}/radio-controller.service")

install(PROGRAMS clients/rx_capture.py clients/tx_tone.py clients/radioctl.py
        DESTINATION ${CMAKE_INSTALL_DATADIR}/radio-controller/clients)

if(PROJECT_IS_TOP_LEVEL OR CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    include(CTest)  # standalone build; in-tree, forgecpp enables testing
endif()
if(BUILD_TESTING OR FORGECPP_BUILD_TESTS)
    if(TARGET forgecpp::testing OR TARGET forgecpp_testing)
        forge_add_test(radio_controller_tests SOURCES tests/test_radio.cpp LIBS radio_core)
        target_compile_definitions(radio_controller_tests PRIVATE
            RADIO_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    endif()
endif()
