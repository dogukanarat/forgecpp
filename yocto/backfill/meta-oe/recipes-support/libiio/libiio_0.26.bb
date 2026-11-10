# Backfill: used only when meta-oe is not in the build. The version of
# third_party/libiio (radio-controller and spectrum-analyzer).
SUMMARY = "libiio - interface to Linux Industrial I/O devices"
HOMEPAGE = "https://wiki.analog.com/resources/tools-software/linux-software/libiio"
SECTION = "libs"
# The library is LGPL; the iio_* tools are GPL.
LICENSE = "LGPL-2.1-only & GPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING.txt;md5=7c13b3376cea0ce68d2d2da0a1b3a72c \
                    file://COPYING_GPL.txt;md5=b234ee4d69f5fce4486a80fdaf4a4263"

SRC_URI = "git://github.com/analogdevicesinc/libiio.git;protocol=https;nobranch=1"
SRCREV = "a0eca0d2bf10326506fb762f0eec14255b27bef5"
S = "${FORGECPP_UNPACKDIR}/git"

DEPENDS = "libxml2"

inherit cmake pkgconfig

# Local (in-kernel IIO), network (iiod) and XML backends, as install.sh builds it.
EXTRA_OECMAKE = " \
    -DWITH_IIOD=OFF -DWITH_TESTS=ON -DWITH_EXAMPLES=OFF -DWITH_DOC=OFF -DWITH_MAN=OFF \
    -DPYTHON_BINDINGS=OFF -DCSHARP_BINDINGS=OFF -DCPP_BINDINGS=OFF -DHAVE_DNS_SD=OFF \
    -DWITH_SERIAL_BACKEND=OFF -DINSTALL_UDEV_RULE=OFF -DWITH_ZSTD=OFF -DWITH_USB_BACKEND=OFF \
    -DOSX_PACKAGE=OFF \
"

# iio_info, iio_readdev, ...: the tools to look at a device.
PACKAGES =+ "${PN}-tests"
FILES:${PN}-tests = "${bindir}"
LICENSE:${PN}-tests = "GPL-2.0-only"
FILES:${PN}-dev += "${libdir}/cmake"
BBCLASSEXTEND = "native nativesdk"
