# forgecpp for Yocto / OpenEmbedded

`recipes-support/forgecpp/forgecpp_git.bb` builds forgecpp and all bundled
libraries with the Yocto toolchain via `install.sh`.

1. Copy `recipes-support/forgecpp` (both recipes: `forgecpp` and
   `forgecpp-protoc-native`) into one of your layers.
2. Pin `SRCREV` to a commit or tag (the recipe defaults to `${AUTOREV}`).
3. Add the runtime library to your image and the SDK:

   ```
   IMAGE_INSTALL:append = " forgecpp forgecpp-forgectl"
   TOOLCHAIN_TARGET_TASK:append = " forgecpp-dev forgecpp-staticdev"
   ```

Packages: `forgecpp` (libraries), `forgecpp-forgectl` (forge.v1 command line
client on the target), `forgecpp-python` (forge.v1 Python modules; needs
python3-protobuf and python3-pyzmq from meta-python), `forgecpp-tools`
(`forgecpp-new`), `forgecpp-iio-tools`, `forgecpp-protoc` (protoc for the
target, rarely needed), `forgecpp-dev` (headers, CMake packages, `.proto` files).

Extra `install.sh` options can be passed with `FORGECPP_INSTALL_ARGS`,
e.g. `FORGECPP_INSTALL_ARGS = "--without-ssl"` or `"--static"`.

The recipe builds ZeroMQ, protobuf and libiio as well; it needs `libxml2`
for libiio's network/XML backends. `protoc` has to run on the build machine
and match the target's libprotobuf exactly, so `forgecpp-protoc-native`
builds it from the same bundled sources (installed as
`${STAGING_BINDIR_NATIVE}/forgecpp-protoc`); forgecpp and every application
with its own `.proto` files use it (`-DFORGE_PROTOC=...`, see below). The libiio command line tools
(`iio_info`, `iio_attr`, ...) are packaged separately in `forgecpp-iio-tools`
because they are GPL-2.0-or-later.

## radio-controller

`recipes-connectivity/radio-controller/radio-controller_git.bb` builds the
AD9364 daemon from `examples/radio_controller` and configures it for the on-board
radio (`backend = "iio"`, `uri = "local:"`) with its state kept in
`/var/lib/radio-controller`. Override with `RADIO_URI = "ip:192.168.2.1"` etc.

```
IMAGE_INSTALL:append = " radio-controller forgecpp-iio-tools"
```

The device tree must describe the AD9364 (`adi,ad9364` with the AXI AD9361
HDL cores), so that the kernel exposes `ad9361-phy`, `cf-ad9361-lpc` and
`cf-ad9361-dds-core-lpc`; `iio_info -u local:` on the target should list them.

## spectrum-analyzer

`recipes-connectivity/spectrum-analyzer/spectrum-analyzer_git.bb` builds the
spectrum analyzer from `examples/spectrum_analyzer` (web UI on port 8081, state
in `/var/lib/spectrum-analyzer`). By default (`SA_BACKEND = "radio"`) it
analyzes RX2 of an AD9361/AD9363 through radio-controller, so both daemons
run together; the unit starts after radio-controller.

Channel roles come from the configuration and must fit the hardware; the
recipes set them from variables:

| Variable | Default | Configuration |
|---|---|---|
| `RADIO_RX_CHANNELS` | `[1]` | radio-controller listens to |
| `RADIO_TX_CHANNEL` | `1` | radio-controller drives |
| `RADIO_TAP_RX` | `[2]` | radio-controller shares (use `[]` on an AD9364) |
| `SA_CHANNEL` | `2` | the analyzer listens to (must be in `RADIO_TAP_RX`) |
| `SA_CONTROL_GAIN` | `true` | the analyzer sets that channel's gain |

```
IMAGE_INSTALL:append = " radio-controller spectrum-analyzer"
```

For a 2R2T transceiver the device tree must not force 1R1T mode
(`adi,1rx-1tx-mode-enable`), so that `cf-ad9361-lpc` has `voltage0` – `voltage3`.
On an AD9364 board set `RADIO_TAP_RX = "[]"`, `SA_BACKEND = "iio"` and
`SA_CHANNEL = "1"`: the analyzer then owns the RX path (and can sweep) and
radio-controller must not run.

## Application recipes

A project created with `forgecpp-new` builds in Yocto without changes:

```bitbake
SUMMARY = "Pump control daemon"
LICENSE = "CLOSED"

SRC_URI = "git://git.example.com/pump-ctl.git;protocol=https;branch=main"
SRCREV = "<commit>"
S = "${WORKDIR}/git"

DEPENDS = "forgecpp forgecpp-protoc-native"
inherit cmake systemd

# the application's .proto files (forge_add_proto) are compiled with this protoc
EXTRA_OECMAKE += "-DFORGE_PROTOC=${STAGING_BINDIR_NATIVE}/forgecpp-protoc"

SYSTEMD_SERVICE:${PN} = "pump-ctl.service"
```

```cmake
find_package(forgecpp REQUIRED)
forge_add_app(pump-ctl
    SOURCES src/main.cpp
    CONFIG  config/pump-ctl.toml        # -> /etc/pump-ctl/pump-ctl.toml
    WEB     web                         # pages compiled into the binary
    SERVICE pump-ctl.service)           # -> /usr/lib/systemd/system
```

The web pages are embedded in the binary, so nothing else has to be
installed and read-only root filesystems work as is. The generated systemd
unit uses `Type=notify` and `WatchdogSec=`; forgecpp reports readiness and
feeds the watchdog without linking libsystemd.

## Without bitbake

Build a tarball with an SDK toolchain and unpack it on the target:

```sh
source /opt/poky/environment-setup-cortexa53-poky-linux
./install.sh --package --prefix /usr --toolchain $OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake --arch aarch64
scp build/forgecpp-0.2.0-aarch64.tar.gz target:/tmp && ssh target tar -xzf /tmp/forgecpp-0.2.0-aarch64.tar.gz -C /
```

## What has been verified

The cross build was verified with an aarch64 GCC toolchain and a toolchain
file that restricts package lookup to the target sysroot, the way OE's does:
all libraries build, the full test suite passes on aarch64 (under qemu), the
example daemons serve their web UI, and an application scaffolded with
`forgecpp-new` cross-compiles against the staged package. The recipe itself
has not yet been run in a full bitbake build.

## Known caveats

- **Source directory on newer releases:** the recipe sets
  `S = "${WORKDIR}/git"`, which is correct up to scarthgap (5.0 LTS). Newer
  releases unpack sources elsewhere; adjust `S` there.
- **Overlap with meta-oe:** forgecpp installs its own copies of the bundled
  libraries (spdlog, nlohmann-json, CLI11, libzmq, cppzmq, libiio, ...) into
  `/usr`. If an image also pulls in the meta-oe recipes for the same libraries,
  bitbake reports files installed by multiple recipes. Use one source for those
  libraries per image (e.g. `FORGECPP_INSTALL_ARGS = "--without-iio"` and
  `DEPENDS += "libiio"` when the image already uses meta-oe's libiio).
- **Reproducibility:** pin `SRCREV`; `${AUTOREV}` follows the branch.
- **Init system:** systemd is supported out of the box. With sysvinit the
  daemons run fine (systemd notifications are no-ops), but you have to provide
  the init script.
- **HTTPS:** the HTTP server is plain HTTP. Configure `[auth]` users on
  network-facing devices and terminate TLS in a reverse proxy if needed.
- **Telnet and forge.v1 endpoints are not encrypted.** The example
  configurations open telnet (`[console].telnet`) and the `[forge]` endpoints
  on all interfaces for the lab. On a fielded device configure `[auth]`
  (telnet then asks for a login and every change needs a role), or bind them
  to `127.0.0.1` / a management interface.
- **protobuf from meta-oe:** do not mix it with forgecpp's bundled protobuf in
  one image (same files); applications must use `forgecpp-protoc`, not
  `protobuf-native` (generated code is tied to the libprotobuf version).
