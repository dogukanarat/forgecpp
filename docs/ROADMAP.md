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

## Plan

Ordered by risk to the running system. Each step lands on its own with tests,
without changing behaviour that is not named.

### 1. The main loop must not take the control channels down with it

*Problem.* `invoke()` waits without a limit. A stuck main loop blocks every
forge.v1 request and every HTTP handler that needs it (8 HTTP threads): the
panel and forgectl stop answering exactly when they are needed for diagnosis.

*Plan.*
- `invoke_for(timeout, fn)`; `ControlService` uses it with
  `[forge].request_timeout_ms` (default 2000) and answers `503 main loop busy`
  (with the loop's lag) instead of waiting.
- Read-only requests that do not need the loop (info, health, metrics, logs,
  audit, trace, streams) stop going through it.
- Metric `forge.control.timeouts`; test: a blocked main loop, forgectl and the
  panel still answer within the timeout.

### 2. One slow request must not block the control endpoint

*Problem.* `[forge].control` is a single REP socket: requests are served one by
one; a trace dump or a request waiting for the main loop holds up everybody.

*Plan.* ROUTER socket with a small worker pool (default 2) and per-request
deadlines; replies keep the request's identity. forge.v1 messages and clients
do not change. Test: a slow request does not delay a concurrent `ping`.

### 3. A transport-neutral error type

*Problem.* `ControlService` (shared by forge.v1, HTTP, console, telnet) throws
`http::HttpError`; ZeroMQ and telnet catch an HTTP type.

*Plan.* `forge::Error{status, message, details}` in `forge/error.hpp`;
`http::HttpError` derives from it (source compatible). Each channel maps it:
HTTP status, `forge.v1.Reply.status`, console text.

### 4. One source for configuration defaults

*Problem.* Defaults are written twice: in the schema and in every
`cfg.get_or(key, default)` (`http.port` 8080 appears three times). They will
drift.

*Plan.* `Config::get<T>(key)` takes the default from the declared schema;
`get_or` stays for undeclared keys. A test walks the sources and fails when a
`get_or` repeats a declared key's default differently. Migrate the framework,
then the examples.

### 5. Split `forge::Application`

*Problem.* `application.cpp` is 1500 lines: HTTP routes, messaging, telnet,
hub registration, persistence, watchdog. Hard to test in parts, every change
touches it.

*Plan.* Move, without behaviour changes, into internal services owned by
`Application`, one per step: `HttpFrontend` (framework routes), `Messaging`
(publishers, control server), `RemoteConsole` (REPL + telnet), `StateStore`
(persist()), `Watchdog` (systemd + health gating), `HubRegistration`.
`Application` keeps the lifecycle and the public API. The existing tests are
the safety net; each service gets its own tests.

### 6. Retire the older JSON control protocol

*Problem.* The REP endpoint also accepts `{"cmd": ...}` JSON: two protocols to
maintain and test.

*Plan.* 0.3: log a warning once per client and count `forge.control.legacy_json`.
0.4: off by default (`[forge].legacy_json = true` to keep it). 0.5: removed.

### 7. A real Python package

*Problem.* The clients find the generated modules with `sys.path` searches.

*Plan.* `forge-v1` Python package (generated `*_pb2.py` + the framing helpers
of `clients/forge_msg.py`), installed into site-packages by CMake and by the
Yocto recipe (`python3-forge-v1`); applications add their own messages the
same way. Clients import it normally.

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
