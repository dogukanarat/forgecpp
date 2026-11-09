SUMMARY = "AD936x radio controller daemon"
DESCRIPTION = "Controls an AD936x (AD9364, AD9361/AD9363) transceiver through libiio and streams IQ \
samples over ZeroMQ. Web UI, forgectl, console and telnet share the same controls. Built on forgecpp."
FORGECPP_APP = "radio-controller"
require recipes-connectivity/forgecpp-app.inc

DEPENDS += "libiio"

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

# Python helpers (rx_capture.py, tx_tone.py) and the radio modules
# need pyzmq, protobuf and numpy (meta-python).
PACKAGES =+ "${PN}-clients"
FILES:${PN}-clients = "${datadir}/radio-controller/clients ${PYTHON_SITEPACKAGES_DIR}/radio"
RDEPENDS:${PN}-clients = "python3-core python3-pyzmq python3-protobuf python3-numpy python3-forge"
