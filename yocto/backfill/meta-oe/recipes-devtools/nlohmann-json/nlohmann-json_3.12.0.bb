# Backfill: used only when meta-oe is not in the build. The version of third_party/json.
SUMMARY = "JSON for Modern C++"
HOMEPAGE = "https://github.com/nlohmann/json"
SECTION = "libs"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE.MIT;md5=3b489645de9825cca5beeb9a7e18b6eb"

SRC_URI = "git://github.com/nlohmann/json.git;protocol=https;nobranch=1"
SRCREV = "55f93686c01528224f448c19128836e7df245f72"
S = "${FORGECPP_UNPACKDIR}/git"

inherit cmake

EXTRA_OECMAKE = "-DJSON_BuildTests=OFF -DJSON_Install=ON -DJSON_MultipleHeaders=ON"

ALLOW_EMPTY:${PN} = "1"
FILES:${PN}-dev += "${datadir}/cmake ${libdir}/cmake ${datadir}/pkgconfig"
RDEPENDS:${PN}-dev = ""
BBCLASSEXTEND = "native nativesdk"
