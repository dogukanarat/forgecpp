SUMMARY = "AD936x spectrum analyzer"
DESCRIPTION = "Spectrum analyzer for AD936x transceivers (RX2 of radio-controller, or its own RX path) with a \
web UI: RBW/VBW, detectors, max/min hold and average traces, markers with peak \
search and peak track, waterfall, channel power and occupied bandwidth, limit \
line alarm. sa.spectrum stream and the command link. Built on forgecpp."
FORGECPP_APP = "spectrum-analyzer"
require recipes-connectivity/forgecpp-app.inc

DEPENDS += "libiio"

# "radio": RX2 of an AD9361/AD9363 shared with radio-controller (both run together).
# "iio": own the RX path of the local IIO device (AD9364; radio-controller must not run).
SA_BACKEND ?= "radio"
SA_URI ?= "local:"
SA_CHANNEL ?= "2"
SA_CONTROL_GAIN ?= "true"

do_install:append() {
    conf=${D}${sysconfdir}/spectrum-analyzer/spectrum-analyzer.toml
    sed -i -e 's|^backend = .*|backend = "${SA_BACKEND}"|' \
           -e 's|^uri = .*|uri = "${SA_URI}"|' \
           -e 's|^state_file = .*|state_file = "/var/lib/spectrum-analyzer/state.json"|' \
           -e 's|^channel = .*|channel = ${SA_CHANNEL}|' \
           -e 's|^control_gain = .*|control_gain = ${SA_CONTROL_GAIN}|' \
           ${conf}
}

SYSTEMD_AUTO_ENABLE:${PN} ?= "enable"
RRECOMMENDS:${PN} = "${@'radio-controller' if d.getVar('SA_BACKEND') == 'radio' else ''}"

# spectrum_watch.py and the sa modules; .proto files are development files.
PACKAGES =+ "${PN}-clients"
FILES:${PN}-clients = "${datadir}/spectrum-analyzer/clients ${PYTHON_SITEPACKAGES_DIR}/sa"
RDEPENDS:${PN}-clients = "python3-core python3-pyzmq python3-protobuf python3-forge"
FILES:${PN}-dev += "${datadir}/spectrum-analyzer/proto"
