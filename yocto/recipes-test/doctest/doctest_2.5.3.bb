# Not in meta-oe (scarthgap): always from this layer. The version of
# third_party/doctest (forgecpp's tests and forgecpp::testing).
SUMMARY = "doctest - the fastest feature-rich C++ single-header testing framework"
HOMEPAGE = "https://github.com/doctest/doctest"
SECTION = "devel"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE.txt;md5=e238ab5cb6e96cb11947e2644e4e8926"

SRC_URI = "git://github.com/doctest/doctest.git;protocol=https;nobranch=1"
SRCREV = "2d0a9359a60c51affe2a9bebb1be1dca47868151"
S = "${FORGECPP_UNPACKDIR}/git"

inherit cmake

EXTRA_OECMAKE = "-DDOCTEST_WITH_TESTS=OFF -DDOCTEST_WITH_MAIN_IN_STATIC_LIB=OFF"

ALLOW_EMPTY:${PN} = "1"
FILES:${PN}-dev += "${datadir}/cmake ${libdir}/cmake"
RDEPENDS:${PN}-dev = ""
BBCLASSEXTEND = "native nativesdk"
