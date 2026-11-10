# Backfill: used only when meta-oe is not in the build. The version of third_party/cppzmq.
SUMMARY = "cppzmq - header-only C++ binding for libzmq"
HOMEPAGE = "https://github.com/zeromq/cppzmq"
SECTION = "libs"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=db174eaf7b55a34a7c89551197f66e94"

SRC_URI = "git://github.com/zeromq/cppzmq.git;protocol=https;nobranch=1"
SRCREV = "3bcbd9dad2f57180aacd4b4aea292a74f0de7ef4"
S = "${FORGECPP_UNPACKDIR}/git"

DEPENDS = "zeromq"

inherit cmake pkgconfig

EXTRA_OECMAKE = "-DCPPZMQ_BUILD_TESTS=OFF"

# Header-only: the package is its headers and CMake config.
ALLOW_EMPTY:${PN} = "1"
FILES:${PN}-dev += "${datadir}/cmake ${libdir}/cmake ${libdir}/pkgconfig ${datadir}/pkgconfig"
RDEPENDS:${PN}-dev = "zeromq-dev"
BBCLASSEXTEND = "native nativesdk"
