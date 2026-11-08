SUMMARY = "16QAM packet modem over an AD936x"
DESCRIPTION = "Ethernet (UDP) <-> 16QAM bursts through radio-controller: 750 ksym/s, \
about 1 MHz wide, up to 2.9 Mbit/s. Preamble detection, frequency and timing \
recovery, phase tracking, CRC-checked packets; web page with the constellation \
and statistics. Built on forgecpp."
HOMEPAGE = "https://github.com/dogukanarat/forgecpp"
SECTION = "network"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://../../LICENSE;md5=d651bd6255f4ea85c3b1b170b8dd19c6"

SRC_URI = "git://github.com/dogukanarat/forgecpp.git;protocol=https;branch=master"
# Pin a release commit for reproducible builds.
SRCREV ?= "${AUTOREV}"
PV = "1.0.0+git${SRCPV}"

S = "${WORKDIR}/git/examples/qam-modem"

# forgecpp provides ZeroMQ and nanopb; forgecpp-protoc-native and
# python3-protobuf-native generate the radio messages.
DEPENDS = "forgecpp forgecpp-protoc-native python3-protobuf-native"

inherit cmake systemd python3native

EXTRA_OECMAKE = " \
    -DBUILD_TESTING=OFF \
    -DFORGE_SYSTEMD_UNIT_DIR=${systemd_system_unitdir} \
    -DFORGE_PROTOC=${STAGING_BINDIR_NATIVE}/forgecpp-protoc \
"

# Carrier and UDP port in /etc/qam-modem/qam-modem.toml
QAM_LO_HZ ?= "2100000000"
QAM_UDP_LISTEN ?= "0.0.0.0:6000"

do_install:append() {
    conf=${D}${sysconfdir}/qam-modem/qam-modem.toml
    sed -i -e 's|^lo_hz = [0-9]*|lo_hz = ${QAM_LO_HZ}|' \
           -e 's|^udp_listen = "[^"]*"|udp_listen = "${QAM_UDP_LISTEN}"|' \
           ${conf}
}

SYSTEMD_SERVICE:${PN} = "qam-modem.service"
# Off by default: it retunes radio-controller and transmits.
SYSTEMD_AUTO_ENABLE:${PN} ?= "disable"
RRECOMMENDS:${PN} = "radio-controller"
CONFFILES:${PN} = "${sysconfdir}/qam-modem/qam-modem.toml"

# echo.py (for the remote PC, but handy on the board too)
PACKAGES =+ "${PN}-clients"
FILES:${PN}-clients = "${datadir}/qam-modem/clients"
RDEPENDS:${PN}-clients = "python3-core"
