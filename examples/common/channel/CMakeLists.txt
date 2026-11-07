# channel: radio channel impairments for complex baseband (frequency offset,
# Doppler, phase noise, clock offset, multipath, fading, IQ imbalance, DC,
# interference, noise). Plain C++; used by channel-emulator and tests.
cmake_minimum_required(VERSION 3.16)

if(TARGET channel_dsp)
    return()
endif()

add_library(channel_dsp STATIC channel.cpp)
target_include_directories(channel_dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(channel_dsp PUBLIC cxx_std_17)
set_target_properties(channel_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(channel_dsp PRIVATE -Wall -Wextra -Wpedantic
        # filters vectorize only with reassociation (NEON on 32-bit ARM is not IEEE exact)
        $<$<NOT:$<CONFIG:Debug>>:-O3 -funsafe-math-optimizations -fno-math-errno -fcx-limited-range>
        # real time even in debug builds (CI runs the end-to-end tests in Debug)
        $<$<CONFIG:Debug>:-O2>)
endif()
