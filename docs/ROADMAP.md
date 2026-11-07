# forgecpp: design fixes plan

Findings of the design review, what is done and the plan for the rest.
Security (authentication defaults, TLS, telnet) is out of scope until the
pre-production security work: forgecpp is used in the lab for now.

## Done

| Finding | Fix |
|---|---|
| Two restart authorities (forge-hub and systemd) | Restarts are systemd's only. `forge_add_app(... RESTART MAX_RESTARTS RESTART_WINDOW RESTART_DELAY WATCHDOG)` generates the unit; forge-hub reads state, `NRestarts`, `Result` and the policy with `systemctl show` and starts / stops / restarts units on request. The hub no longer starts processes or reads their environment. Hangs: systemd's watchdog (the main loop feeds it). |
| Slow, timing-dependent hub tests (60 s) | systemd behind an interface with a fake in tests, 20 ms ticks, sleep processes instead of applications: 2 s. Whole suite: 66 s -> 5 s. |
| Sanitizers only run by hand | CI job `sanitizers`: ASan+UBSan and TSan, the bundled libraries instrumented too, all tests. ccache for every CI job. |
| `invoke()` shared exception objects between threads (TSan) | Errors are described on the main loop and thrown anew on the caller's thread. |
| A stuck main loop hung forgectl and the panel | `invoke_for(timeout, fn)`: forge.v1 and the framework's HTTP routes wait at most `[forge].request_timeout_ms` (2000) for the main loop, then answer `503 main loop busy` with the loop's lag (metric `forge.control.timeouts`); a request that timed out is never applied later. info, health, metrics, logs, audit, trace, alarms and streams do not need the loop. |
| `[forge].control` served one request at a time (REP) | `zmq::Server` is a ROUTER socket with an I/O thread and a worker pool (`[forge].control_workers`, 2); replies go back to the client that asked, requests that waited too long for a worker (or beyond 256 waiting) get 503 without being handled. Clients and messages unchanged. |
| The control service threw an HTTP type (`http::HttpError`) on every channel | `forge::Error{status, message, details}` in `forge/error.hpp`, thrown by `ControlService`, `web::Dashboard`, forge-hub and `invoke_for`; HTTP, forge.v1 and the console map it. `http::HttpError` derives from it (throwing it still works; `catch (const http::HttpError&)` around control calls becomes `catch (const forge::Error&)`). |
| Defaults written twice (schema and every `get_or`) | `Schema::apply_defaults()` fills in every declared key after loading and reloading; declared keys are read with `cfg.value<T>(key)` (framework, forge-hub, all examples migrated; the small examples declare their keys now). Environment-dependent defaults are `.computed()`. A test walks `src/`, `hub/src/`, `examples/` and fails on `get_or` of a declared key. |
| `application.cpp` was 1600 lines doing everything | Internal services in `src/app/` own their state: `HttpFrontend` (server, framework routes, web events and logs), `LayoutStore`, `Messaging` (events, streams, control endpoint), `RemoteConsole` (commands, REPL, telnet), `StateStore`, `Watchdog`, `HubRegistration`. `Application` keeps the lifecycle and the public API (900 lines). `StateStore`, `LayoutStore` and `Watchdog` have their own tests. |
| Two control protocols (forge.v1 and `{"cmd": ...}` JSON) | 0.3: the JSON one is counted (`forge.control.legacy_json`), each client is named in a warning once, `[forge].legacy_json = false` refuses it (410). 0.4: off by default. 0.5: removed. |
| Python clients found the generated modules with `sys.path` searches | `forge-v1` package (`forge.v1.*_pb2`, `forge.v1.framing`) installed into site-packages (`FORGE_PYTHON_SITEDIR`: the interpreter's directory for the prefix; Yocto `python3-forge-v1`); applications use `forge_install_python()` for theirs (radio.v1, sa.v1, the scaffolder's). Clients import normally; `forge_msg.py` and every path search are gone. |

## Plan

Ordered by risk to the running system. Each step lands on its own with tests,
without changing behaviour that is not named.

### 8. Versions and compatibility

*Plan.* Semantic versions with a CHANGELOG; 0.3.0 for this release. SOVERSION
follows the minor version until 1.0. CI check that `proto/forge/v1` only
changes compatibly (no renumbered or retyped fields; removed ones reserved),
by comparing descriptors with the last release.

### 9. Test the paths that are not tested yet

- **systemd.** GitHub runners have systemd: a CI job installs radio-controller
  and forge-hub as units, kills the application, and checks the restart, the
  hub's view, `start-limit-hit` and the hub's restart button.
- **Yocto.** A nightly job builds `forgecpp`, `forgecpp-hub` and the example
  recipes with kas for qemuarm64 and boots the image in QEMU.
- **Deterministic time in applications.** Let `Application` take a
  `Scheduler::manual_clock` in tests, so timer-driven logic (alarms with
  hysteresis, rate limits) is tested without sleeping.

### 10. forge-hub follow-ups

- Several instances of one application (`name@instance`, matching systemd
  template units).
- One port for everything: a reverse proxy under `/apps/<name>/` (needs the
  pages to use relative URLs; forge.js gets a base path).

## Pre-production security work (later)

Secure defaults (no `[auth]`: control, telnet and the forge.v1 endpoints only on
localhost), hub registration over a Unix socket with `SO_PEERCRED`, TLS (or a
terminating proxy) for HTTP, telnet replaced or tunnelled through ssh, a review
of the roles of every command.
