# Running it: access control, observability, forge-hub

Who may change what, what the program tells about itself, and the dashboard of every application on a device.

## Access control, leases and the audit trail

Every way in (web, commands over ZeroMQ or HTTP, console, telnet) goes through
`forge::ControlService`, which applies, in order: the caller's role, leases,
validation, the change, the audit record and the state event.

```toml
[auth]
enabled = true
default_role = "viewer"          # without credentials: read only
[auth.users.alice]
password = "..."                 # web (Basic auth), telnet login
role = "admin"
[auth.users.ci]
token = "..."                    # forgectl --token over HTTP, Authorization: Bearer
role = "operator"
```

- **Roles:** `viewer` (read), `operator` (set, press, call, ack, lease),
  `admin` (log levels, controls/commands marked `.role(forge::Role::admin)`).
  Without `[auth]` everyone is admin (lab default); the legacy `[http.auth]`
  user becomes an admin. c2 frames carry no credentials, so commands on a
  link get `default_role`: with `[auth]` on, change things over HTTP, or keep
  the link endpoint on a trusted network.
- **Leases:** `forgectl lease rx_lo_mhz 60` (or `group:Radio`, or the panel's
  Leases page) keeps others from changing a control while you work with it;
  others get `409 leased by ...`.
- **Audit:** who (channel, client, user) changed what, old → new value,
  result and trace id: `forgectl audit`, the panel's Audit page, the
  `audit` event topic and optionally a JSON-lines file.
- **Persistence:** `.persist()` on a control keeps its value in `[state].file`.
- **Commands (RPC):** typed parameters with defaults and ranges:

```cpp
g.action("scan", "Scan", {forge::web::number_param("from", 2400).range(70, 6000),
                           forge::web::number_param("to")},
          [this](const forge::json& a) { return scan(a["from"], a["to"]); }).confirm("Start a scan?");
```

## Observability

| | |
|---|---|
| **Metrics** | `metrics().counter / gauge / histogram / gauge_fn`, with labels (`counter("rx.frames", {{"channel", "1"}})`); the framework adds HTTP, main loop (lag, busy, queue depth, dropped tasks), thread pool, the links and streams (`forge.link.*`, `forge.stream.*`, `forge.serial.*`), alarms, health and process metrics. Prometheus text at `GET /metrics` (dots become underscores: `forge_loop_lag_seconds`), `forgectl metrics`, the panel's Metrics page (with trends), the `metrics` event topic |
| **Trace** | `FORGE_TRACE_SCOPE("name")`, `forge::trace::instant/counter`: a lock-free flight recorder per thread; main loop tasks and control requests are traced automatically. Download Chrome JSON (`forgectl trace`, panel Trace page, `GET /forge/api/trace?seconds=5`) and open it in ui.perfetto.dev |
| **Crash reports** | Fatal signals write a report (signal, backtrace, last log lines) to `[crash].dir`; the next start raises the `forge.crashed` alarm with its path |
| **Logs** | `[log] format = "json"` for collectors; per-logger levels (`forgectl log-level radio debug`); history via `forgectl logs`, live via `watch` and telnet `logs on` |
| **Health** | `health().set("radio", forge::HealthState::failed, "device lost")`; `/health` (`?strict=1` → 503), `forgectl health`, the panel's Health page; with `health.watchdog_grace_s` a failed critical component stops the systemd watchdog pings so systemd restarts the daemon |

## Ports on a device

Every program binds what its configuration names; two programs with the same
configuration collide. The examples follow one block of ten ports per program
and HTTP from 8080 up; the table and the `ipc://` alternative are in
[field-notes.md](field-notes.md#ports-on-a-device).

## forge-hub: every application on the device in one place

`forge-hub` (in `hub/`, installed with forgecpp, optional) is the device's
front page: open `http://<device>/` and see every application, its state,
health, alarms, uptime, CPU and memory, its systemd restart policy and
restarts, with **Open** / **Control panel** buttons, so nobody has to remember
ports.

```
application ──register (pid, ports)──▶ forge-hub  tcp://127.0.0.1:5550 (localhost)
            ◀──GET /health────────────
systemd ◀──systemctl show / restart / stop / start── forge-hub
browser ──▶ http://<device>/ ──links──▶ http://<device>:<port>/ of each application
```

- **Optional for the applications.** Every forgecpp application tries to
  register when it starts, on its own thread, and carries on whether or not a
  hub answers. It registers again every `retry_s` (30 s), so a hub started
  later, or restarted, finds everyone. `hub.retry_s = 0` = one attempt only;
  `hub.register = false` = never.
- **Restarts are systemd's job.** The application declares its policy where it
  is built; `forge_add_app` turns it into the unit:

  ```cmake
  forge_add_app(radio-controller SOURCES src/main.cpp CONFIG config/radio-controller.toml
      RESTART on-failure     # never | on-failure | always            -> Restart=
      MAX_RESTARTS 3         # within RESTART_WINDOW, then the unit fails -> StartLimitBurst=4
      RESTART_WINDOW 600     #                                          -> StartLimitIntervalSec=
      RESTART_DELAY 2        #                                          -> RestartSec=
      WATCHDOG 30)           # hung main loop: killed and restarted      -> WatchdogSec=
  ```

  The unit also has what a daemon never needs taken away: `NoNewPrivileges`,
  `PrivateTmp`, `ProtectSystem=full` (state goes to `StateDirectory`),
  `ProtectHome=read-only`, `ProtectControlGroups`, `ProtectKernelModules`,
  `ProtectKernelLogs`, `ProtectClock`, `ProtectHostname`,
  `ProtectProc=invisible`, `RestrictNamespaces`, `RestrictSUIDSGID`,
  `RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK`,
  `SystemCallArchitectures=native`, `LockPersonality`. Devices (`/dev/tty*`,
  IIO), `/sys` writes and real-time priorities stay allowed. `NO_HARDENING`
  leaves them out. The unit runs as root unless `USER <name>` or
  `DYNAMIC_USER` is given; both drop every capability except those named with
  `CAPABILITIES` (`CAP_NET_BIND_SERVICE` for a port below 1024). It starts
  after `network.target`; `NETWORK_ONLINE` waits for routes and addresses
  (`network-online.target`) for a program that connects out at start.

  A crash or a hang is restarted by systemd with or without the hub; after too
  many restarts the unit is `failed` and stays down. The hub reads it all from
  systemd (`systemctl show`: state, `NRestarts`, `Result`, the policy) and
  shows it: "killed by a signal; systemd restarts it in 2 s", "gave up after
  3 restarts in 600 s (last: killed by a signal)". Crashes and failed units are alarms on the hub
  (`[hub] critical = true` makes a failed unit a critical one).
- **Operators** restart, stop and start units from the page,
  `forgectl -e tcp://<device>:5551 call restart name=radio-controller` or the
  hub's own `/forge/` panel (`systemctl reset-failed` + `restart`), with roles
  and the audit trail of every forgecpp application. Remote panels get
  `forge.hub.app_status` messages on the hub's `apps` stream.
- **Applications without a unit** (started by hand, in development) are listed
  and checked too; the hub reports when they end ("ended without
  unregistering", "exited with code 3") but cannot restart them. A stop request
  (`SIGTERM`, Ctrl-C) unregisters with that reason and is never reported as a crash.
- **Several instances** of one application (two radios): `forge_add_app(...
  TEMPLATE)` also generates `<name>@.service`; `systemctl start
  radio-controller@rx2` runs it with `/etc/radio-controller/rx2.toml`,
  `--instance rx2` and its own state directory, and the hub lists it as
  `radio-controller@rx2` ("radio-controller (rx2)") next to the others.
  `--instance` (or `$FORGE_INSTANCE`) does the same without systemd.
- **One port for the device**: each application's pages are also at
  `http://<device>/apps/<name>/` (`[proxy].enabled`, on by default), and the
  dashboard links there, so only port 80 has to be reachable. Live updates
  (Server-Sent Events) are streamed through. Pages work under any prefix when
  they use relative URLs (`forge/forge.js`, `assets/...`) and `forge.api('/api/x')`
  / `forge.url('/api/x')` for their own routes: forge.js takes the
  application's root from where it was loaded.
- **Other web interfaces** on the device can be listed without registering:
  `[static.camera] url = "http://{host}:8554/"`, `health_url = ...`.

```sh
sudo forge-hub -c /etc/forge-hub/forge-hub.toml     # or: systemctl enable --now forge-hub
```

The hub needs root for port 80 and `systemctl`; it starts no processes itself.
