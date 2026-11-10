# Not in meta-oe (scarthgap): always from this layer. The version of third_party/tomlplusplus.
SUMMARY = "toml++ - header-only TOML config file parser for C++17"
HOMEPAGE = "https://marzer.github.io/tomlplusplus"
SECTION = "libs"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=90960f22c10049c117d56ed2ee5ee167"

SRC_URI = "git://github.com/marzer/tomlplusplus.git;protocol=https;nobranch=1"
SRCREV = "30172438cee64926dc41fdd9c11fb3ba5b2ba9de"
S = "${FORGECPP_UNPACKDIR}/git"

inherit cmake

EXTRA_OECMAKE = "-DBUILD_EXAMPLES=OFF"

ALLOW_EMPTY:${PN} = "1"
FILES:${PN}-dev += "${datadir}/cmake ${libdir}/cmake ${datadir}/pkgconfig"
RDEPENDS:${PN}-dev = ""
BBCLASSEXTEND = "native nativesdk"
