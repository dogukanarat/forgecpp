# forgecpp for Yocto / OpenEmbedded

`recipes-support/forgecpp/forgecpp_git.bb` builds forgecpp and all bundled
libraries with the Yocto toolchain via `install.sh`.

1. Copy `recipes-support/forgecpp` into one of your layers.
2. Pin `SRCREV` to a commit or tag (the recipe defaults to `${AUTOREV}`).
3. Add the runtime library to your image and the SDK:

   ```
   IMAGE_INSTALL:append = " forgecpp"
   TOOLCHAIN_TARGET_TASK:append = " forgecpp-dev forgecpp-staticdev"
   ```

Extra `install.sh` options can be passed with `FORGECPP_INSTALL_ARGS`,
e.g. `FORGECPP_INSTALL_ARGS = "--without-ssl"` or `"--static"`.

## Application recipes

A project created with `forgecpp-new` builds in Yocto without changes:

```bitbake
SUMMARY = "Pump control daemon"
LICENSE = "CLOSED"

SRC_URI = "git://git.example.com/pump-ctl.git;protocol=https;branch=main"
SRCREV = "<commit>"
S = "${WORKDIR}/git"

DEPENDS = "forgecpp"
inherit cmake systemd

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
  libraries (spdlog, nlohmann-json, CLI11, ...) into `/usr`. If an image also
  pulls in the meta-oe recipes for the same libraries, bitbake reports files
  installed by multiple recipes. Use one source for those libraries per image.
- **Reproducibility:** pin `SRCREV`; `${AUTOREV}` follows the branch.
- **Init system:** systemd is supported out of the box. With sysvinit the
  daemons run fine (systemd notifications are no-ops), but you have to provide
  the init script.
- **HTTPS:** the HTTP server is plain HTTP. Configure `[http.auth]` on
  network-facing devices and terminate TLS in a reverse proxy if needed.
