# forgecpp for Yocto / OpenEmbedded

This directory is a layer (`meta-forgecpp`, `conf/layer.conf`) for a BSP:
forgecpp, forge-hub, the developer tools and the example applications. It
needs meta-oe and meta-python (for `zeromq`, `cppzmq`, `nlohmann-json`,
`cli11`, `tomlplusplus`, `doctest`, `protobuf`, `libiio`, `python3-protobuf`,
`python3-pyzmq`) and brings the three libraries they do not have (`quill`,
`cpp-httplib`, `concurrentqueue`). Every library is its own package, with its
own license and CVE record, and is shared by every program on the device.

```
bblayers.conf:  BBLAYERS += "/path/to/forgecpp/yocto"
local.conf:     IMAGE_INSTALL:append = " forgecpp forgecpp-forgectl forgecpp-hub"
```

## The recipes

| Recipe | Packages | What |
|---|---|---|
| `recipes-support/forgecpp/forgecpp_git.bb` | `forgecpp`, `forgecpp-dev`, `forgecpp-forgectl`, `forgecpp-hub`, `python3-forge`, `forgecpp-ptest` | libforgecpp (nanopb and linenoise compiled in), forgectl, forge-hub (PACKAGECONFIG `hub`, on by default), the Python package, the test programs (PACKAGECONFIG `ptest`) |
| `recipes-devtools/forgecpp-tools/forgecpp-tools_git.bb` | `forgecpp-tools`, `nativesdk-forgecpp-tools` | `forgecpp-new`, `forge-proto.py` and nanopb's generator: for the SDK (an application with its own `.proto` files) and, if wanted, the target |
| `recipes-support/quill`, `cpp-httplib`, `concurrentqueue` | the libraries meta-oe lacks | the versions forgecpp is built and tested with (its submodules) |
| `recipes-connectivity/*` | `radio-controller`, `spectrum-analyzer`, `qam-modem`, `channel-emulator` (+ `-clients`) | the example applications, from `recipes-connectivity/forgecpp-app.inc` |

The source and the commit are in one place, `recipes-support/forgecpp/forgecpp-src.inc`:
`tools/release.sh` writes the release's commit into `FORGECPP_SRCREV`, so a
released layer builds exactly that commit. Between releases it is
`${AUTOREV}` (the head of `FORGECPP_BRANCH`); pin it in local.conf for a
reproducible build of a development commit, or build a checkout with
`externalsrc`, as `kas/qemuarm64.yml` does.

`LAYERSERIES_COMPAT` is scarthgap, the release the kas file builds. The
recipes take the unpack directory from `FORGECPP_UNPACKDIR` (`conf/layer.conf`:
`${WORKDIR}` on scarthgap, `${UNPACKDIR}` from styhead on), so they are
written for styhead and walnascar too; add a series to `LAYERSERIES_COMPAT`
once a build of it has passed. The recipes of this change were read against
the scarthgap classes; they have not been through a bitbake build or
`yocto-check-layer` yet.

## Try it in QEMU (kas)

`kas/qemuarm64.yml` builds a qemuarm64 image (systemd) with forgecpp,
forge-hub, radio-controller on its simulator, spectrum-analyzer, qam-modem,
channel-emulator and the ptest package, from this checkout:

```sh
pip install kas
FORGECPP_SRC=$PWD kas build yocto/kas/qemuarm64.yml                # the image
FORGECPP_SRC=$PWD kas build yocto/kas/qemuarm64.yml -c testimage   # boot in QEMU + runtime tests
```

The runtime tests (`lib/oeqa/runtime/cases/forgecpp.py`) check that the units
are active, forge-hub lists the applications, `forgectl` reaches
radio-controller and the `forge` Python package imports; `ptest-runner
forgecpp` runs the framework's own tests on the target. The `yocto` workflow
runs this nightly (and on demand).

## The SDK

```
TOOLCHAIN_TARGET_TASK:append = " forgecpp-dev"
TOOLCHAIN_HOST_TASK:append = " nativesdk-forgecpp-tools nativesdk-protobuf nativesdk-python3-protobuf"
```

`forgecpp-dev` alone is enough for `find_package(forgecpp)` (the test helper
`forgecpp::testing` is an INTERFACE target with its `main()` as a source file,
not a static library). `nativesdk-forgecpp-tools` gives the SDK
`forgecpp-new` and the message generator; `forge_add_proto` finds the SDK's
`protoc` by itself (`OECORE_NATIVE_SYSROOT`). An application then builds as on
a workstation:

```sh
. /opt/poky/5.0/environment-setup-cortexa53-crypto-poky-linux
forgecpp-new my-daemon && cd my-daemon
cmake -B build && cmake --build build -j
```

## An application recipe

```
SUMMARY = "my-daemon"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=..."
SRC_URI = "git://...;protocol=https;branch=main"
SRCREV = "..."
S = "${WORKDIR}/git"          # styhead and later: ${UNPACKDIR}/git
DEPENDS = "forgecpp protobuf-native python3-protobuf-native"   # the last two: own .proto files
inherit cmake systemd python3-dir python3native
EXTRA_OECMAKE = "-DBUILD_TESTING=OFF -DFORGE_PROTOC=${STAGING_BINDIR_NATIVE}/protoc \
                 -DFORGE_SYSTEMD_UNIT_DIR=${systemd_system_unitdir} \
                 -DFORGE_PYTHON_SITEDIR=${PYTHON_SITEPACKAGES_DIR}"
SYSTEMD_SERVICE:${PN} = "my-daemon.service"
CONFFILES:${PN} = "${sysconfdir}/my-daemon/my-daemon.toml"
```

`forge_add_app` generates the unit from the policy in CMakeLists.txt
(`RESTART`, `MAX_RESTARTS`, `WATCHDOG`, `USER`/`DYNAMIC_USER`, `CAPABILITIES`,
`NETWORK_ONLINE`); a recipe changes a policy with a drop-in
(`<unit>.d/override.conf`) instead of patching the unit.

## forge-hub (optional)

```
IMAGE_INSTALL:append = " forgecpp-hub"
FORGECPP_HUB_PORT = "80"                          # optional, default 80
PACKAGECONFIG:remove:pn-forgecpp = "hub"          # leave it out of the build entirely
```

Every forgecpp application on the device registers with it by itself
(configured by its own `[hub]` section) and is restarted by its systemd unit,
whose state the hub shows. Port 80 needs root or `CAP_NET_BIND_SERVICE`
(`forge_add_app(... CAPABILITIES CAP_NET_BIND_SERVICE DYNAMIC_USER)`).

## The example applications

`radio-controller` (`RADIO_BACKEND`, `RADIO_URI`, `RADIO_RX_CHANNELS`,
`RADIO_TX_CHANNEL`, `RADIO_TAP_RX`), `spectrum-analyzer` (`SA_BACKEND`,
`SA_URI`, `SA_CHANNEL`, `SA_CONTROL_GAIN`), `qam-modem` (`QAM_LO_HZ`,
`QAM_UDP_LISTEN`; unit installed, not enabled) and `channel-emulator` (unit
installed, not enabled) set their configuration from these variables in
`do_install:append`; `IMAGE_INSTALL:append = " radio-controller
spectrum-analyzer"` puts both on the image. The device tree must describe the
transceiver (`adi,ad9364` with the AXI AD9361 HDL cores, not forced to 1R1T
for a 2R2T part), so that the kernel exposes `ad9361-phy`, `cf-ad9361-lpc`
and `cf-ad9361-dds-core-lpc`; `iio_info -u local:` on the target should list
them. The libiio command line tools come from meta-oe's `libiio-iiod` /
`libiio` packages.
