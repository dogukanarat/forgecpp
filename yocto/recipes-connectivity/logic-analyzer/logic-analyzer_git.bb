SUMMARY = "Four-channel logic analyzer with a web UI"
DESCRIPTION = "Logic analyzer for a Zynq-7000 board (Arty Z7) with a capture core in the FPGA \
that sends changes instead of samples: waveforms with zoom, cursors and measurements, \
UART / I2C / SPI decoders, a data table and terminal view, triggers, saved captures, \
VCD / CSV / protocol log exports. Built on forgecpp."
FORGECPP_APP = "logic-analyzer"
require recipes-connectivity/forgecpp-app.inc

# "uio": the capture core (examples/logic-analyzer/fpga, a UIO node in the
# device tree); "sim": the simulator (no FPGA needed).
LA_SOURCE ?= "uio"
LA_UIO_DEVICE ?= "logic-analyzer"

do_install:append() {
    conf=${D}${sysconfdir}/logic-analyzer/logic-analyzer.toml
    sed -i -e 's|^source = .*|source = "${LA_SOURCE}"|' \
           -e 's|^uio_device = .*|uio_device = "${LA_UIO_DEVICE}"|' \
           -e 's|^captures_dir = .*|captures_dir = "/var/lib/logic-analyzer/captures"|' \
           -e 's|^analyzers_file = .*|analyzers_file = "/var/lib/logic-analyzer/analyzers.json"|' \
           -e 's|^file = .*|file = "/var/lib/logic-analyzer/state.json"|' \
           ${conf}
}

SYSTEMD_AUTO_ENABLE:${PN} ?= "enable"
# The UIO driver for the capture core's node ("generic-uio").
RRECOMMENDS:${PN} = "${@'kernel-module-uio-pdrv-genirq' if d.getVar('LA_SOURCE') == 'uio' else ''}"
