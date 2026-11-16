SUMMARY = "cpp-httplib - C++ HTTP/HTTPS server and client library"
HOMEPAGE = "https://github.com/yhirose/cpp-httplib"
SECTION = "libs"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=1321bdf796c67e3a8ab8e352dd81474b"

SRC_URI = "git://github.com/yhirose/cpp-httplib.git;protocol=https;branch=master"
SRCREV = "7e5d0683ac326788e52ce3c44585c1af24b04532"
S = "${FORGECPP_UNPACKDIR}/git"

# Compiled (HTTPLIB_COMPILE) as a shared library: forgecpp links it, so an
# application does not compile the whole header. The feature macros (SSL,
# zlib) are part of the class layout; forgecpp's CMake package and .pc file
# carry them to every consumer.
DEPENDS = "openssl zlib python3-native"

inherit cmake

EXTRA_OECMAKE = " \
    -DHTTPLIB_COMPILE=ON \
    -DHTTPLIB_SHARED=ON \
    -DHTTPLIB_REQUIRE_OPENSSL=ON \
    -DHTTPLIB_REQUIRE_ZLIB=ON \
    -DHTTPLIB_USE_BROTLI_IF_AVAILABLE=OFF \
    -DHTTPLIB_USE_ZSTD_IF_AVAILABLE=OFF \
    -DHTTPLIB_USE_NON_BLOCKING_GETADDRINFO=ON \
    -DHTTPLIB_TEST=OFF \
"

FILES:${PN}-dev += "${libdir}/cmake"
# 0.65 also installs README.md (docdir) and LICENSE (datadir/licenses).
FILES:${PN}-doc += "${datadir}/licenses"
BBCLASSEXTEND = "native nativesdk"
