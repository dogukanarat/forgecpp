# Shared by the example applications of forgecpp (one source, one commit):
# the recipe sets SUMMARY, DESCRIPTION, FORGECPP_APP (the directory under
# examples/) and its own configuration variables.
require recipes-support/forgecpp/forgecpp-src.inc
HOMEPAGE = "https://github.com/dogukanarat/forgecpp"
SECTION = "network"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "${FORGECPP_LICENSE_CHKSUM}"
PV = "1.0.0"

# S is the repository root (forgecpp-src.inc); the application is one directory of it.
OECMAKE_SOURCEPATH = "${S}/examples/${FORGECPP_APP}"

# forgecpp (and through it ZeroMQ, nanopb); protoc and nanopb's generator on
# the build machine for the application's own messages.
DEPENDS = "forgecpp protobuf-native python3-protobuf-native"

inherit cmake systemd python3-dir python3native

EXTRA_OECMAKE = " \
    -DBUILD_TESTING=OFF \
    -DFORGE_PYTHON_SITEDIR=${PYTHON_SITEPACKAGES_DIR} \
    -DFORGE_SYSTEMD_UNIT_DIR=${systemd_system_unitdir} \
    -DFORGE_PROTOC=${STAGING_BINDIR_NATIVE}/protoc \
"

SYSTEMD_SERVICE:${PN} = "${FORGECPP_APP}.service"
CONFFILES:${PN} = "${sysconfdir}/${FORGECPP_APP}/${FORGECPP_APP}.toml"
