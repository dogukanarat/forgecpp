SUMMARY = "protoc for forgecpp builds (build machine)"
DESCRIPTION = "The protobuf compiler from forgecpp's bundled protobuf (v21.12). \
Generated code must match the target's libprotobuf exactly, so forgecpp and \
the applications built on it use this protoc rather than another protobuf-native."
HOMEPAGE = "https://github.com/dogukanarat/forgecpp"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=37b5762e07f0af8c74ce80a8bda4266b"

SRC_URI = "gitsm://github.com/dogukanarat/forgecpp.git;protocol=https;branch=master"
# Use the same commit as forgecpp.
SRCREV ?= "${AUTOREV}"
PV = "21.12+git${SRCPV}"

S = "${WORKDIR}/git/third_party/protobuf"

inherit cmake native

EXTRA_OECMAKE = " \
    -Dprotobuf_BUILD_TESTS=OFF \
    -Dprotobuf_BUILD_EXAMPLES=OFF \
    -Dprotobuf_WITH_ZLIB=OFF \
    -Dprotobuf_BUILD_SHARED_LIBS=OFF \
"

# Only the compiler is needed on the build machine.
do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/protoc ${D}${bindir}/forgecpp-protoc
    ln -sf forgecpp-protoc ${D}${bindir}/protoc
}
