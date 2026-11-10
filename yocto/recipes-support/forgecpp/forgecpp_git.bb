SUMMARY = "forgecpp - batteries-included C++20 framework for Linux daemons"
DESCRIPTION = "An application skeleton for embedded Linux daemons: web control panel, c2/tmtc \
commands (wire compatible with zephyrus) over ZeroMQ and serial lines, streams, forgectl, telnet \
console, metrics, tracing, audit trail, systemd integration. Links the distribution's ZeroMQ, \
cpp-httplib, quill and the header-only libraries; nanopb and linenoise are compiled in."
SECTION = "libs"
require forgecpp-src.inc
PV = "${FORGECPP_PV}"

# forgecpp (MIT) with what is compiled in: nanopb (Zlib), linenoise (BSD-2-Clause),
# BS::thread_pool (MIT), GridStack.js (MIT, in the web pages). The libraries it
# links are packaged by their own recipes with their own licenses.
LICENSE = "MIT & Zlib & BSD-2-Clause"
LIC_FILES_CHKSUM = " \
    ${FORGECPP_LICENSE_CHKSUM} \
    file://third_party/nanopb/LICENSE.txt;md5=9db4b73a55a3994384112efcdb37c01f \
    file://third_party/linenoise/LICENSE;md5=faa55ac8cbebebcb4a84fe1ea2879578 \
    file://third_party/thread-pool/LICENSE.txt;md5=47ca9b80e8129537a2b9e401c5d9f3fe \
    file://src/web/vendor/gridstack/LICENSE;md5=edb97b6dc3fdc07939ff20bd634870d5 \
"
CVE_PRODUCT = "forgecpp"

# The libraries: meta-oe's zeromq, cppzmq, nlohmann-json, cli11, tomlplusplus,
# doctest, protobuf (protoc on the build machine); this layer's quill,
# cpp-httplib and concurrentqueue. The messages are generated on the build
# machine with protoc and nanopb's generator (python3-protobuf); the target
# gets C code only.
DEPENDS = " \
    zeromq cppzmq nlohmann-json cli11 tomlplusplus quill cpp-httplib concurrentqueue \
    openssl zlib \
    protobuf-native python3-protobuf-native \
"

inherit cmake pkgconfig systemd python3-dir python3native ptest

# forge-hub, the dashboard of the device's applications: built with the "hub"
# PACKAGECONFIG, installed only when the image asks for it:
#   IMAGE_INSTALL:append = " forgecpp-hub"
# Remove it from the build with PACKAGECONFIG:remove:pn-forgecpp = "hub".
#
# The optional components, each in a package of its own (forgecpp-sqlite,
# forgecpp-lz4): an image only gets SQLite or liblz4 when an application
# links them. PACKAGECONFIG:remove:pn-forgecpp = "sqlite" leaves one out.
PACKAGECONFIG ??= "hub sqlite lz4 ${@bb.utils.contains('DISTRO_FEATURES', 'ptest', 'ptest', '', d)}"
PACKAGECONFIG[hub] = "-DFORGECPP_BUILD_HUB=ON,-DFORGECPP_BUILD_HUB=OFF"
PACKAGECONFIG[sqlite] = "-DFORGECPP_WITH_SQLITE=ON,-DFORGECPP_WITH_SQLITE=OFF,sqlite3"
PACKAGECONFIG[lz4] = "-DFORGECPP_WITH_LZ4=ON,-DFORGECPP_WITH_LZ4=OFF,lz4"
PACKAGECONFIG[ptest] = "-DFORGECPP_BUILD_TESTS=ON,-DFORGECPP_BUILD_TESTS=OFF,doctest"
FORGECPP_HUB_PORT ?= "80"

EXTRA_OECMAKE = " \
    -DFORGECPP_BUILD_EXAMPLES=OFF \
    -DFORGECPP_WARNINGS=OFF \
    -DFORGECPP_INSTALL_TOOLS=OFF \
    -DFORGE_PROTOC=${STAGING_BINDIR_NATIVE}/protoc \
    -DFORGE_SYSTEMD_UNIT_DIR=${systemd_system_unitdir} \
    -DFORGE_PYTHON_SITEDIR=${PYTHON_SITEPACKAGES_DIR} \
"

do_install:append() {
    if [ -f ${D}${sysconfdir}/forge-hub/forge-hub.toml ]; then
        sed -i -e 's|^port = 80 |port = ${FORGECPP_HUB_PORT} |' ${D}${sysconfdir}/forge-hub/forge-hub.toml
    fi
}

# The framework's own test programs on the target (ptest -r forgecpp).
do_install_ptest() {
    install -d ${D}${PTEST_PATH}/tests
    for t in forgecpp_tests forgecpp_pb_tests forgecpp_zmq_tests forgecpp_framework_tests forgecpp_storage_tests \
             fuzz_c2 fuzz_pb fuzz_serial; do
        [ -f ${B}/tests/$t ] && install -m 0755 ${B}/tests/$t ${D}${PTEST_PATH}/tests/
    done
}

PACKAGES =+ "${PN}-hub ${PN}-forgectl ${PN}-sqlite ${PN}-lz4 python3-forge"
#   forgecpp-hub       forge-hub: dashboard of the device's applications (state, health, systemd restarts)
#   forgecpp-sqlite    forgecpp::sqlite: SQLite and the store-and-forward backlog
#   forgecpp-lz4       forgecpp::lz4: LZ4 frames
#   forgecpp-forgectl  command line client (forgectl) for the target
#   python3-forge      Python package "forge": messages, stream framing, c2 clients (site-packages)
FILES:${PN}-hub = "${bindir}/forge-hub ${sysconfdir}/forge-hub ${systemd_system_unitdir}/forge-hub.service"
CONFFILES:${PN}-hub = "${sysconfdir}/forge-hub/forge-hub.toml"
RDEPENDS:${PN}-hub = "${PN}"
SYSTEMD_PACKAGES = "${@bb.utils.contains('PACKAGECONFIG', 'hub', '${PN}-hub', '', d)}"
SYSTEMD_SERVICE:${PN}-hub = "forge-hub.service"
FILES:${PN}-forgectl = "${bindir}/forgectl"
FILES:${PN}-sqlite = "${libdir}/libforgecpp-sqlite${SOLIBS}"
FILES:${PN}-lz4 = "${libdir}/libforgecpp-lz4${SOLIBS}"
LICENSE:${PN}-sqlite = "MIT"
LICENSE:${PN}-lz4 = "MIT"
FILES:python3-forge = "${PYTHON_SITEPACKAGES_DIR}/forge ${PYTHON_SITEPACKAGES_DIR}/nanopb_pb2.py ${PYTHON_SITEPACKAGES_DIR}/pyproject.toml"
RDEPENDS:python3-forge = "python3-core python3-protobuf"
# The ZeroMQ client (forge.c2) imports pyzmq when it is used; the serial and
# CAN clients do not. pyzmq is in meta-python, recommended when that layer is
# in the build (a missing recommendation is an error for bitbake).
RRECOMMENDS:python3-forge = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'meta-python', 'python3-pyzmq', '', d)}"
RPROVIDES:python3-forge = "python3-forge-v1"
RREPLACES:python3-forge = "python3-forge-v1"
RCONFLICTS:python3-forge = "python3-forge-v1"
# Development: headers, the CMake package (forgecpp::testing is an INTERFACE
# target with testing_main.cpp as its source: no static library), pkg-config,
# the .proto files and the message generator.
FILES:${PN}-dev += "${libdir}/cmake ${datadir}/forgecpp ${includedir}/forgecpp"
RDEPENDS:${PN}-dev += "doctest-dev"
RDEPENDS:${PN}-ptest += "python3-core"
LICENSE:${PN}-dev = "MIT"
LICENSE:${PN}-forgectl = "MIT"
LICENSE:${PN}-hub = "MIT"
LICENSE:python3-forge = "MIT"
