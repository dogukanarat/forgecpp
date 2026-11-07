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
| No versions or compatibility rules | 0.3.0 with a CHANGELOG; SOVERSION and `find_package` compatibility follow the minor version until 1.0. `proto/forge/v1/API.json` records the released forge.v1 API; `tools/proto-compat.py` fails CI on renumbered, retyped, renamed or unreserved removed fields and enum values. |
| Restarts, start limits and the hub's view only tested against a fake systemd | CI job `systemd`: forge-hub and radio-controller installed as real units; crash -> restart -> the hub counts it, the start limit -> failed -> the hub says it gave up, the hub's restart and stop buttons. It found two bugs (the Result after an automatic restart is refused, NOTIFY_SOCKET leaking into the hub's children). |
| No Yocto build in CI | `yocto/` is a layer; `yocto/kas/qemuarm64.yml` builds an image from the checkout and boots it with runtime tests; nightly workflow `yocto`. |
| Zynq-7000 (armhf) build not verified | `cmake/toolchains/zynq7000-armhf.cmake`; CI job `cross-armhf` cross-builds everything (host protoc), packages it and runs radio-controller and forgectl under QEMU user emulation. |
| One application could run only once per device in forge-hub | `name@instance`: `--instance` / `$FORGE_INSTANCE` / the template unit's instance; `forge_add_app(... TEMPLATE)` generates `<name>@.service` (config `<instance>.toml`, own StateDirectory); `AppRegistration.instance`; the hub keeps instances apart. |
| Every application's page needed its own port | forge-hub proxies `/apps/<name>/...` (all methods, query strings, redirects kept under the prefix, Server-Sent Events streamed) to the application's port; the dashboard links there (`[proxy].enabled`). forge.js finds the application's root from its own URL (`forge.base`, `forge.url()`); the built-in and example pages use relative URLs. |
| Docs drifted from the code | A sweep of README, the examples' and Yocto READMEs, header comments, configuration comments and the scaffolder: REP -> ROUTER, `streams()`/`BufferPool` examples, the generated project tree, `[hub]` restart wording, the Yocto application example (generated unit), build paths. Found one bug: spectrum-analyzer's `settle_buffers` default depends on the backend and is now `.computed()`. The defaults check also covers the scaffolder's template. |

## Plan

Ordered by risk to the running system. Each step lands on its own with tests,
without changing behaviour that is not named.

### 9. Deterministic time in applications

Let `Application` take a `Scheduler::manual_clock` in tests, so timer-driven
logic (alarms with hysteresis, rate limits) is tested without sleeping.

## Pre-production security work (later)

Secure defaults (no `[auth]`: control, telnet and the forge.v1 endpoints only on
localhost), hub registration over a Unix socket with `SO_PEERCRED`, TLS (or a
terminating proxy) for HTTP, telnet replaced or tunnelled through ssh, a review
of the roles of every command.
