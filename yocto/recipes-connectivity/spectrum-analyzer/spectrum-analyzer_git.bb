SUMMARY = "AD936x spectrum analyzer"
DESCRIPTION = "Spectrum analyzer for AD936x transceivers (RX2 of radio-controller, or its own RX path) with a \
web UI: RBW/VBW, detectors, max/min hold and average traces, markers with peak \
search and peak track, waterfall, channel power and occupied bandwidth, limit \
line alarm. sa.v1.Spectrum stream and forge.v1 control. Built on forgecpp."
HOMEPAGE = "https://github.com/dogukanarat/forgecpp"
SECTION = "network"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://../../LICENSE;md5=d651bd6255f4ea85c3b1b170b8dd19c6"

SRC_URI = "git://github.com/dogukanarat/forgecpp.git;protocol=https;branch=master"
# Pin a release commit for reproducible builds.
SRCREV ?= "${AUTOREV}"
PV = "1.0.0+git${SRCPV}"

S = "${WORKDIR}/git/examples/spectrum_analyzer"

# forgecpp provides ZeroMQ, protobuf and libiio (built from its submodules);
# forgecpp-protoc-native generates the application's messages (radio.v1, sa.v1).
DEPENDS = "forgecpp forgecpp-protoc-native"

inherit cmake systemd

EXTRA_OECMAKE = " \
    -DBUILD_TESTING=OFF \
    -DFORGE_SYSTEMD_UNIT_DIR=${systemd_system_unitdir} \
    -DFORGE_PROTOC=${STAGING_BINDIR_NATIVE}/forgecpp-protoc \
"

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

SYSTEMD_SERVICE:${PN} = "spectrum-analyzer.service"
SYSTEMD_AUTO_ENABLE:${PN} ?= "enable"
RRECOMMENDS:${PN} = "${@'radio-controller' if d.getVar('SA_BACKEND') == 'radio' else ''}"
CONFFILES:${PN} = "${sysconfdir}/spectrum-analyzer/spectrum-analyzer.toml"

# spectrum_watch.py and the sa.v1 modules; .proto files are development files.
PACKAGES =+ "${PN}-clients"
FILES:${PN}-clients = "${datadir}/spectrum-analyzer/clients ${datadir}/spectrum-analyzer/python"
RDEPENDS:${PN}-clients = "python3-core python3-pyzmq python3-protobuf forgecpp-python"
FILES:${PN}-dev += "${datadir}/spectrum-analyzer/proto"
