# Backfill: used only when meta-oe (openembedded-layer) is not in the build
# (conf/layer.conf, BBFILES_DYNAMIC). The version of third_party/libzmq.
SUMMARY = "ZeroMQ - high-performance asynchronous messaging library"
HOMEPAGE = "https://zeromq.org"
SECTION = "libs"
LICENSE = "MPL-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=9741c346eef56131163e13b9db1241b3"

SRC_URI = "git://github.com/zeromq/libzmq.git;protocol=https;nobranch=1"
SRCREV = "622fc6dde99ee172ebaa9c8628d85a7a1995a21d"
S = "${FORGECPP_UNPACKDIR}/git"

inherit cmake pkgconfig

EXTRA_OECMAKE = " \
    -DBUILD_SHARED=ON -DBUILD_STATIC=OFF -DBUILD_TESTS=OFF -DWITH_PERF_TOOL=OFF -DWITH_DOCS=OFF \
    -DENABLE_DRAFTS=OFF -DENABLE_PRECOMPILED=OFF -DWITH_LIBSODIUM=OFF -DENABLE_CURVE=OFF -DWITH_LIBBSD=OFF \
"

FILES:${PN}-dev += "${datadir}/cmake ${libdir}/cmake"
# AUTHORS, NEWS and LICENSE (.txt), installed to share/zmq.
FILES:${PN}-doc += "${datadir}/zmq"
BBCLASSEXTEND = "native nativesdk"
