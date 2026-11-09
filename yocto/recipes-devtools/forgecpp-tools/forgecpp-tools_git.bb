SUMMARY = "forgecpp developer tools: the scaffolder and the message generator"
DESCRIPTION = "forgecpp-new (a new application from a template) and forge-proto.py with nanopb's \
generator (nanopb C structs and the forgecpp names from a .proto file). For the SDK \
(nativesdk-forgecpp-tools: an application with its own messages builds against the SDK's protoc \
and python3-protobuf) and, if wanted, the target."
SECTION = "devel"
require recipes-support/forgecpp/forgecpp-src.inc
PV = "${FORGECPP_PV}"
LICENSE = "MIT & Zlib"
LIC_FILES_CHKSUM = " \
    ${FORGECPP_LICENSE_CHKSUM} \
    file://third_party/nanopb/LICENSE.txt;md5=9db4b73a55a3994384112efcdb37c01f \
"

inherit allarch

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${bindir} ${D}${datadir}/forgecpp/nanopb
    install -m 0755 ${S}/tools/forgecpp-new ${D}${bindir}/forgecpp-new
    install -m 0755 ${S}/tools/forge-proto.py ${D}${datadir}/forgecpp/forge-proto.py
    cp -R ${S}/third_party/nanopb/generator ${D}${datadir}/forgecpp/nanopb/
    find ${D}${datadir}/forgecpp/nanopb -name '*.bat' -delete -o -name '*py2*' -delete -o -name '__pycache__' -prune -exec rm -rf {} +
}

FILES:${PN} = "${bindir}/forgecpp-new ${datadir}/forgecpp"
RDEPENDS:${PN} = "bash python3-core python3-protobuf protobuf"
RDEPENDS:${PN}:class-nativesdk = "nativesdk-bash nativesdk-python3-core nativesdk-python3-protobuf nativesdk-protobuf"
BBCLASSEXTEND = "native nativesdk"
