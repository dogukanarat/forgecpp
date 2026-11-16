SUMMARY = "sqlite_orm - SQLite ORM for modern C++, header-only"
DESCRIPTION = "Maps C++ structs to SQLite tables; queries are C++ expressions. \
Used by forgecpp::orm (forge/orm.hpp)."
HOMEPAGE = "https://github.com/fnc12/sqlite_orm"
SECTION = "libs"
# From 1.7 on, AGPL-3.0, or MIT bought from its author (COMM-LICENSE). The
# header is compiled into the program that includes it. A distribution with
# INCOMPATIBLE_LICENSE = "AGPL-3.0*" leaves forgecpp's "orm" PACKAGECONFIG out.
LICENSE = "AGPL-3.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=4ae09d45eac4aa08d013b5f2e01c67f6"

# The version forgecpp is built and tested with (third_party/sqlite_orm).
# meta-oe has sqlite-orm 1.5 (BSD-3-Clause, C++14): this one is newer, and
# wins at the same layer priority.
SRC_URI = "git://github.com/fnc12/sqlite_orm.git;protocol=https;branch=master"
SRCREV = "5f1a2ce84a3d72711b4f0a440fdaba977868ae67"
S = "${FORGECPP_UNPACKDIR}/git"

DEPENDS = "sqlite3"

inherit cmake

# Its tests would download Catch2.
EXTRA_OECMAKE = "-DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF"

# Header-only: the package is its CMake config and the header.
ALLOW_EMPTY:${PN} = "1"
FILES:${PN}-dev += "${libdir}/cmake"
# oe-core packages the SQLite library as libsqlite3 (headers: libsqlite3-dev).
RDEPENDS:${PN}-dev = "libsqlite3-dev"
BBCLASSEXTEND = "native nativesdk"
