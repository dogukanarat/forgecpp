# ad936x: AD9364/AD936x access shared by the radio examples.
#   - Backend interface with a libiio implementation (ADI ad9361 driver)
#     and a simulator for development without hardware
#   - DSP helpers (FFT, windows, power spectra)
#   - radio.v1 messages (proto/radio/v1/radio.proto)
#
# Used with add_subdirectory() from radio-controller and spectrum-analyzer.
cmake_minimum_required(VERSION 3.16)

if(TARGET ad936x)
    return()
endif()

# libiio (0.x API). Without it only the simulator backend is available.
option(AD936X_WITH_IIO "Build the libiio backend for AD936x hardware" ON)
set(AD936X_HAS_IIO OFF)
if(AD936X_WITH_IIO)
    find_path(IIO_INCLUDE_DIR iio.h)
    find_library(IIO_LIBRARY NAMES iio libiio)
    if(IIO_INCLUDE_DIR AND IIO_LIBRARY)
        set(AD936X_HAS_IIO ON)
    else()
        message(WARNING "ad936x: libiio not found - only the simulator backend is available")
    endif()
endif()
set(AD936X_HAS_IIO ${AD936X_HAS_IIO} PARENT_SCOPE)
message(STATUS "ad936x: libiio backend ${AD936X_HAS_IIO}")

add_library(ad936x STATIC backend.cpp sim_backend.cpp dsp.cpp)
target_include_directories(ad936x PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ad936x PUBLIC cxx_std_20)
set_target_properties(ad936x PROPERTIES POSITION_INDEPENDENT_CODE ON)
# nlohmann/json comes with forgecpp
if(TARGET forgecpp::forgecpp)
    target_link_libraries(ad936x PUBLIC forgecpp::forgecpp)
else()
    target_link_libraries(ad936x PUBLIC forgecpp)
endif()

if(AD936X_HAS_IIO)
    target_sources(ad936x PRIVATE iio_backend.cpp)
    target_include_directories(ad936x PRIVATE ${IIO_INCLUDE_DIR})
    target_link_libraries(ad936x PUBLIC ${IIO_LIBRARY})
    target_compile_definitions(ad936x PUBLIC RADIO_HAS_IIO=1)
    if(IIO_LIBRARY MATCHES "\\.a$")
        # A static libiio does not carry its own dependencies.
        find_package(LibXml2 REQUIRED)
        find_package(Threads REQUIRED)
        target_link_libraries(ad936x PUBLIC LibXml2::LibXml2 Threads::Threads)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
            if(LIBUSB_FOUND)
                target_link_libraries(ad936x PUBLIC PkgConfig::LIBUSB)
            endif()
        endif()
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ad936x PRIVATE -Wall -Wextra -Wpedantic)
endif()

# radio.v1 messages (IqBlock, TxBlock): C++ for the applications, Python for the clients.
set(AD936X_PYTHON_DIR "${CMAKE_CURRENT_BINARY_DIR}/python" CACHE INTERNAL "radio.v1 Python modules")
forge_add_proto(ad936x ROOT ${CMAKE_CURRENT_SOURCE_DIR}/proto PROTOS radio/v1/radio.proto
                PYTHON ${AD936X_PYTHON_DIR})
