# forge-hub

The front page of a device: every forgecpp application that runs on it, with
its state, health, alarms, uptime, CPU and memory, its systemd restart policy
and restarts, and **Open** / **Control panel** buttons, so nobody has to
remember ports. Built and installed with forgecpp (`FORGECPP_BUILD_HUB`, on by
default; Yocto: the `hub` PACKAGECONFIG, package `forgecpp-hub`).

```
application ──register (pid, ports)──▶ forge-hub  tcp://127.0.0.1:5550 (localhost)
            ◀──GET /health────────────
systemd ◀──systemctl show / restart / stop / start── forge-hub
browser ──▶ http://<device>/ ──links / proxy──▶ each application's pages
```

- **Applications register themselves** ([hub] in their configuration:
  `register`, `endpoint`, `retry_s`, `critical`, `group`, `title`); they carry
  on without a hub, and register again when one appears.
- **Restarts are systemd's.** The hub reads each unit's state, `NRestarts`
  and `Result` with `systemctl show`, and starts, stops and restarts units on
  request (`forgectl -e tcp://device:5551 call restart name=radio-controller`).
  It never supervises processes itself.
- **One port for everything:** with `[proxy].enabled` the hub proxies
  `/apps/<name>/...` to each application's HTTP port, Server-Sent Events
  included, so a device needs one open port.
- **Instances:** `radio-controller@rx2` and `@rx3` are kept apart.
- **Its own streams and events:** `apps` (`forge.hub.app_status`, one message
  per application) on `[forge].data`, alarms for crashes and units that gave
  up on `[forge].events`.

Configuration: [`config/forge-hub.toml`](config/forge-hub.toml) (port 80 needs
`AmbientCapabilities=CAP_NET_BIND_SERVICE` or a higher port). Protocol:
[`proto/forge/hub.proto`](../proto/forge/hub.proto). Tests: `hub/tests`
(a fake systemd, 2 s). More: [docs/operations.md](../docs/operations.md#forge-hub-every-application-on-the-device-in-one-place).
