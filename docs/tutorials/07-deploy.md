# 7. Deploying: systemd, forge-hub, Yocto

**systemd.** The restart policy is part of the build:

```cmake
forge_add_app(pump SOURCES src/main.cpp src/pump.cpp
    CONFIG pump.toml                                   # -> /etc/pump/pump.toml
    RESTART on-failure MAX_RESTARTS 3 WATCHDOG 30)     # -> pump.service
```

`cmake --install` puts the binary, its configuration and the generated unit
in place: `Type=notify`, the watchdog (a hung main loop is killed and
restarted), at most 3 restarts in 10 minutes, then the unit stays failed. `TEMPLATE`
adds `pump@.service` for several instances (`/etc/pump/<instance>.toml`).

```sh
sudo systemctl enable --now pump
journalctl -u pump -f
```

**forge-hub** (optional, `forgecpp-hub` package) is the device's dashboard on
port 80. Every forgecpp application registers by itself (`[hub]`); the hub
shows health, alarms, CPU and memory, the unit's state, restarts and why it
failed, starts / stops / restarts units, and serves each application's pages
under `http://<device>/apps/pump/`.

**Yocto.** `yocto/` is a layer (`meta-forgecpp`). An application recipe:

```bitbake
DEPENDS = "forgecpp forgecpp-protoc-native"
inherit cmake systemd
EXTRA_OECMAKE += "-DFORGE_PROTOC=${STAGING_BINDIR_NATIVE}/forgecpp-protoc \
                  -DFORGE_SYSTEMD_UNIT_DIR=${systemd_system_unitdir}"
SYSTEMD_SERVICE:${PN} = "pump.service"
```

Try a whole image in QEMU: `FORGECPP_SRC=$PWD kas build yocto/kas/qemuarm64.yml`.
Cross builds without Yocto: `install.sh --toolchain
cmake/toolchains/zynq7000-armhf.cmake --package`. Before production: the
security work in [docs/ROADMAP.md](../ROADMAP.md).
