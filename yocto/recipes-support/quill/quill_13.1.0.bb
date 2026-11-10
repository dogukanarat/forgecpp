SUMMARY = "Quill - asynchronous low-latency C++ logging library"
HOMEPAGE = "https://github.com/odygrd/quill"
SECTION = "libs"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b6da680e78b545725b930cedf82789d3"

# The version forgecpp is built and tested with (third_party/quill).
SRC_URI = "git://github.com/odygrd/quill.git;protocol=https;branch=master"
SRCREV = "78c0696c5be589488d882c280796dfa06bdcd492"
S = "${FORGECPP_UNPACKDIR}/git"

inherit cmake

# Header-only: the package is its CMake config and the headers.
ALLOW_EMPTY:${PN} = "1"
FILES:${PN}-dev += "${libdir}/cmake ${libdir}/pkgconfig"
RDEPENDS:${PN}-dev = ""
BBCLASSEXTEND = "native nativesdk"
