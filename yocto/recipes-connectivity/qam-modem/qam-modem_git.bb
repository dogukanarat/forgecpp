SUMMARY = "16QAM packet modem over an AD936x"
DESCRIPTION = "Ethernet (UDP) <-> 16QAM bursts through radio-controller: 750 ksym/s, \
about 1 MHz wide, up to 2.9 Mbit/s. Preamble detection, frequency and timing \
recovery, phase tracking, CRC-checked packets; web page with the constellation \
and statistics. Built on forgecpp."
FORGECPP_APP = "qam-modem"
require recipes-connectivity/forgecpp-app.inc

# Carrier and UDP port in /etc/qam-modem/qam-modem.toml
QAM_LO_HZ ?= "2100000000"
QAM_UDP_LISTEN ?= "0.0.0.0:6000"

do_install:append() {
    conf=${D}${sysconfdir}/qam-modem/qam-modem.toml
    sed -i -e 's|^lo_hz = [0-9]*|lo_hz = ${QAM_LO_HZ}|' \
           -e 's|^udp_listen = "[^"]*"|udp_listen = "${QAM_UDP_LISTEN}"|' \
           ${conf}
}

# Off by default: it retunes radio-controller and transmits.
SYSTEMD_AUTO_ENABLE:${PN} ?= "disable"
RRECOMMENDS:${PN} = "radio-controller"

# echo.py (for the remote PC, but handy on the board too)
PACKAGES =+ "${PN}-clients"
FILES:${PN}-clients = "${datadir}/qam-modem/clients"
RDEPENDS:${PN}-clients = "python3-core"
