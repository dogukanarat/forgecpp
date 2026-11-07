# Cross toolchain for Zynq-7000 (Z7010/Z7020: dual Cortex-A9, NEON, hard float)
# with Debian/Ubuntu's arm-linux-gnueabihf compiler (g++-arm-linux-gnueabihf):
#
#   ./install.sh --toolchain cmake/toolchains/zynq7000-armhf.cmake --package --without-ssl --without-zlib
#
# A Yocto / PetaLinux SDK brings its own toolchain file (OEToolchainConfig.cmake):
# use that one instead, it also knows the target's sysroot.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard")

set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
