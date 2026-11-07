SUMMARY = "AD9364 spectrum analyzer"
DESCRIPTION = "Swept/realtime spectrum analyzer on the AD9364 (AD936x) RX path with a \
web UI: RBW/VBW, detectors, max/min hold and average traces, markers with peak \
search and peak track, waterfall, channel power and occupied bandwidth, limit \
line alarm. Spectrum stream and control over ZeroMQ. Built on forgecpp."
HOMEPAGE = "https://github.com/dogukanarat/forgecpp"
SECTION = "network"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://../../LICENSE;md5=d651bd6255f4ea85c3b1b170b8dd19c6"

SRC_URI = "git://github.com/dogukanarat/forgecpp.git;protocol=https;branch=master"
# Pin a release commit for reproducible builds.
SRCREV ?= "${AUTOREV}"
PV = "1.0.0+git${SRCPV}"

S = "${WORKDIR}/git/examples/spectrum_analyzer"

# forgecpp provides forgecpp::zmq and libiio (built from its submodules).
DEPENDS = "forgecpp"

inherit cmake systemd

EXTRA_OECMAKE = " \
    -DBUILD_TESTING=OFF \
    -DFORGE_SYSTEMD_UNIT_DIR=${systemd_system_unitdir} \
"

# On the board the radio is the local IIO device; keep runtime changes across reboots.
SA_BACKEND ?= "iio"
SA_URI ?= "local:"

do_install:append() {
    conf=${D}${sysconfdir}/spectrum-analyzer/spectrum-analyzer.toml
    sed -i -e 's|^backend = .*|backend = "${SA_BACKEND}"|' \
           -e 's|^uri = .*|uri = "${SA_URI}"|' \
           -e 's|^state_file = .*|state_file = "/var/lib/spectrum-analyzer/state.json"|' \
           ${conf}
}

SYSTEMD_SERVICE:${PN} = "spectrum-analyzer.service"
# Both daemons use the AD9364 RX path (the unit conflicts with radio-controller):
# enable the one the image is for.
SYSTEMD_AUTO_ENABLE:${PN} ?= "enable"
CONFFILES:${PN} = "${sysconfdir}/spectrum-analyzer/spectrum-analyzer.toml"
