SUMMARY = "Radio channel emulator for the TX path of radio-controller"
DESCRIPTION = "Noise at an SNR, frequency offset and Doppler, phase noise, sample clock \
error, multipath, Rayleigh/Rician fading, IQ imbalance, DC offset and interference, \
applied to the TX samples between a sender (qam-modem) and radio-controller. \
Controls and presets on the web panel and forgectl. Built on forgecpp."
HOMEPAGE = "https://github.com/dogukanarat/forgecpp"
SECTION = "network"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://../../LICENSE;md5=d651bd6255f4ea85c3b1b170b8dd19c6"

SRC_URI = "git://github.com/dogukanarat/forgecpp.git;protocol=https;branch=master"
# Pin a release commit for reproducible builds.
SRCREV ?= "${AUTOREV}"
PV = "1.0.0+git${SRCPV}"

S = "${WORKDIR}/git/examples/channel-emulator"

# forgecpp provides ZeroMQ and nanopb; forgecpp-protoc-native and
# python3-protobuf-native generate the radio messages.
DEPENDS = "forgecpp forgecpp-protoc-native python3-protobuf-native"

inherit cmake systemd python3native

EXTRA_OECMAKE = " \
    -DBUILD_TESTING=OFF \
    -DFORGE_SYSTEMD_UNIT_DIR=${systemd_system_unitdir} \
    -DFORGE_PROTOC=${STAGING_BINDIR_NATIVE}/forgecpp-protoc \
"

SYSTEMD_SERVICE:${PN} = "channel-emulator.service"
# Off by default: it only does something when a sender's TX points at it.
SYSTEMD_AUTO_ENABLE:${PN} ?= "disable"
RRECOMMENDS:${PN} = "radio-controller"
CONFFILES:${PN} = "${sysconfdir}/channel-emulator/channel-emulator.toml"
