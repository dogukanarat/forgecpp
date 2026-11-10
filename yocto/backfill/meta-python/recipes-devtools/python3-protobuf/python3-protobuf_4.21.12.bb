# Backfill: used only when meta-python is not in the build. Matches
# third_party/protobuf (21.12). forgecpp's message generator needs it on the
# build machine (python3-protobuf-native); python3-forge on the target.
SUMMARY = "Protocol Buffers for Python"
HOMEPAGE = "https://github.com/protocolbuffers/protobuf"
SECTION = "devel/python"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=37b5762e07f0af8c74ce80a8bda4266b"

inherit pypi setuptools3

PYPI_PACKAGE = "protobuf"
SRC_URI[sha256sum] = "7cd532c4566d0e6feafecc1059d04c7915aec8e182d1cf7adee8b24ef1e2e6ab"

# The pure Python implementation (no C++ extension to build).
RDEPENDS:${PN} += " \
    python3-datetime python3-json python3-logging python3-netclient python3-numbers \
    python3-pkgutil python3-unittest \
"
BBCLASSEXTEND = "native nativesdk"
