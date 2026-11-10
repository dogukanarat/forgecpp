# Backfill: used only when meta-oe is not in the build. The version of
# third_party/protobuf. forgecpp needs protoc on the build machine
# (protobuf-native) and in the SDK (nativesdk-protobuf-compiler); nothing on
# the target links libprotobuf (the messages use nanopb).
SUMMARY = "Protocol Buffers - structured data serialisation mechanism"
HOMEPAGE = "https://github.com/protocolbuffers/protobuf"
SECTION = "console/tools"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=37b5762e07f0af8c74ce80a8bda4266b"

SRC_URI = "git://github.com/protocolbuffers/protobuf.git;protocol=https;nobranch=1"
SRCREV = "f0dc78d7e6e331b8c6bb2d5283e06aa26883ca7c"
S = "${FORGECPP_UNPACKDIR}/git"

DEPENDS = "zlib"

inherit cmake pkgconfig

EXTRA_OECMAKE = " \
    -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_EXAMPLES=OFF -Dprotobuf_BUILD_SHARED_LIBS=ON \
    -Dprotobuf_BUILD_PROTOC_BINARIES=ON -Dprotobuf_WITH_ZLIB=ON \
"

# The same split as meta-oe's recipe: protoc in -compiler, the lite runtime apart.
PACKAGES =+ "${PN}-compiler ${PN}-lite"
FILES:${PN}-compiler = "${bindir} ${libdir}/libprotoc${SOLIBS}"
FILES:${PN}-lite = "${libdir}/libprotobuf-lite${SOLIBS}"
FILES:${PN}-dev += "${libdir}/cmake"
BBCLASSEXTEND = "native nativesdk"
