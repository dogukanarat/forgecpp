# Backfill: used only when meta-oe is not in the build. The version of third_party/CLI11.
SUMMARY = "CLI11 - command line parser for C++11 and beyond"
HOMEPAGE = "https://github.com/CLIUtils/CLI11"
SECTION = "libs"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=744e78b25a90f01b35b087d9fb985ed4"

SRC_URI = "git://github.com/CLIUtils/CLI11.git;protocol=https;nobranch=1"
SRCREV = "cbd58a3696887b34c70949aef21a71735a0c2ad5"
S = "${FORGECPP_UNPACKDIR}/git"

inherit cmake

EXTRA_OECMAKE = "-DCLI11_BUILD_TESTS=OFF -DCLI11_BUILD_EXAMPLES=OFF -DCLI11_BUILD_DOCS=OFF -DCLI11_PRECOMPILED=OFF"

ALLOW_EMPTY:${PN} = "1"
FILES:${PN}-dev += "${datadir}/cmake ${libdir}/cmake ${datadir}/pkgconfig"
RDEPENDS:${PN}-dev = ""
BBCLASSEXTEND = "native nativesdk"
