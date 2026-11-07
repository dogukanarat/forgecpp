SUMMARY = "AD936x radio controller daemon"
DESCRIPTION = "Controls an AD936x (AD9364, AD9361/AD9363) transceiver through libiio and streams IQ \
samples over ZeroMQ. Web UI, console and ZeroMQ control share the same controls. \
Built on forgecpp."
HOMEPAGE = "https://github.com/dogukanarat/forgecpp"
SECTION = "network"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://../../LICENSE;md5=d651bd6255f4ea85c3b1b170b8dd19c6"

SRC_URI = "git://github.com/dogukanarat/forgecpp.git;protocol=https;branch=master"
# Pin a release commit for reproducible builds.
SRCREV ?= "${AUTOREV}"
PV = "1.0.0+git${SRCPV}"

S = "${WORKDIR}/git/examples/radio_controller"

# forgecpp provides forgecpp::zmq and libiio (built from its submodules).
DEPENDS = "forgecpp"

inherit cmake systemd

EXTRA_OECMAKE = " \
    -DBUILD_TESTING=OFF \
    -DFORGE_SYSTEMD_UNIT_DIR=${systemd_system_unitdir} \
"

# On the board the radio is the local IIO device; keep runtime changes across reboots.
RADIO_BACKEND ?= "iio"
RADIO_URI ?= "local:"
# Channel roles (must fit the transceiver: AD9364 = RX1/TX1 only)
RADIO_RX_CHANNELS ?= "[1]"
RADIO_TX_CHANNEL ?= "1"
RADIO_TAP_RX ?= "[2]"

do_install:append() {
    conf=${D}${sysconfdir}/radio-controller/radio-controller.toml
    sed -i -e 's|^backend = .*|backend = "${RADIO_BACKEND}"|' \
           -e 's|^uri = .*|uri = "${RADIO_URI}"|' \
           -e 's|^state_file = .*|state_file = "/var/lib/radio-controller/state.json"|' \
           -e 's|^rx_channels = .*|rx_channels = ${RADIO_RX_CHANNELS}|' \
           -e 's|^tx_channel = .*|tx_channel = ${RADIO_TX_CHANNEL}|' \
           -e 's|^tap_rx = .*|tap_rx = ${RADIO_TAP_RX}|' \
           ${conf}
}

SYSTEMD_SERVICE:${PN} = "radio-controller.service"
CONFFILES:${PN} = "${sysconfdir}/radio-controller/radio-controller.toml"

# Python helpers (rx_capture.py, tx_tone.py, radioctl.py) need pyzmq and numpy (meta-python).
PACKAGES =+ "${PN}-clients"
FILES:${PN}-clients = "${datadir}/radio-controller/clients"
RDEPENDS:${PN}-clients = "python3-core python3-json python3-pyzmq python3-numpy"
