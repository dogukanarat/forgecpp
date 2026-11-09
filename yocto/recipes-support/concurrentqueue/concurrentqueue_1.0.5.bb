SUMMARY = "moodycamel::ConcurrentQueue - lock-free queues for C++"
HOMEPAGE = "https://github.com/cameron314/concurrentqueue"
SECTION = "libs"
LICENSE = "BSD-2-Clause | BSL-1.0"
LIC_FILES_CHKSUM = "file://LICENSE.md;md5=3e3bd5d3367ca1cdb53ff11ab44322ea"

SRC_URI = "git://github.com/cameron314/concurrentqueue.git;protocol=https;branch=master"
SRCREV = "9afb99746f0f5fc94ac8aef737053ae0481ba8d1"
S = "${UNPACKDIR}/git"

inherit cmake

# Header-only: the CMake config and the headers (include/concurrentqueue/).
ALLOW_EMPTY:${PN} = "1"
FILES:${PN}-dev += "${libdir}/cmake ${datadir}/cmake"
RDEPENDS:${PN}-dev = ""
BBCLASSEXTEND = "native nativesdk"
