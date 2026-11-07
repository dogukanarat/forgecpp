# Changelog

forgecpp follows [semantic versioning](https://semver.org). Until 1.0 a minor
version may break the C++ API: the library's SOVERSION is `<major>.<minor>`
and `find_package(forgecpp 0.3)` only accepts 0.3.x. The forge.v1 protocol
(`proto/forge/v1`) only changes compatibly; CI compares it with the API of
the last release (`proto/forge/v1/API.json`, `tools/proto-compat.py`).

## 0.3.0

### Added

- **forge.v1**: one protobuf message standard for every application
  (`proto/forge/v1`): control requests and replies, events (state, alarms,
  log, audit, health, metrics) and the applications' own streams, over
  ZeroMQ (`[forge].control`, `events`, `data`) and HTTP (`/forge/api/rpc`).
- **forgectl**: command line client for forge.v1 (get/set/press/call,
  alarms, logs, metrics, trace, audit, health, streams, leases).
- **Control**: roles and users (`[auth]`), leases, an audit trail, commands
  with typed parameters, persistent controls, component health with the
  systemd watchdog.
- **Observability**: metrics (Prometheus `/metrics`), a trace flight recorder
  (Chrome/Perfetto JSON), structured JSON logs, crash reports with a
  backtrace.
- **Telnet console** sharing the local console's commands, with logins.
- **forge-hub** (optional; Yocto `PACKAGECONFIG` "hub"): dashboard of the
  device's applications on port 80. Applications register by themselves;
  the hub shows their health, alarms, systemd state, restarts and policy, and
  starts / stops / restarts their units.
- **Generated systemd units**: `forge_add_app(... RESTART MAX_RESTARTS
  RESTART_WINDOW RESTART_DELAY WATCHDOG)`; restarting is systemd's job.
- **Web pages**: cards on a grid, movable and resizable, layouts stored on the
  device (`[web].layout_file`); a phone shows them as a list.
- `invoke_for(timeout, fn)`; requests answer `503 main loop busy` after
  `[forge].request_timeout_ms` instead of hanging (`forge.control.timeouts`).
- `forge::Error{status, message, details}` (`forge/error.hpp`), independent of
  the channel; `http::HttpError` derives from it.
- `Config::value<T>(key)`: declared keys take their default from the schema
  (`Schema::apply_defaults`); `Key::computed()`.
- Python package **forge-v1** (`forge.v1.*_pb2`, `forge.v1.framing`) in
  site-packages; `forge_install_python()` for applications' messages; Yocto
  `python3-forge-v1`.
- `[forge].control_workers`: the control endpoint serves requests in parallel.
- `forge::is_standard_name()`: daemon names are lowercase words separated by
  '-' (warning otherwise); `forgecpp-new` converts names.
- Examples: spectrum-analyzer (AD936x, web UI) running next to
  radio-controller; sensor-service, pipeline and thermostat declare their
  configuration; qam-modem (16QAM packet modem through radio-controller,
  UDP in and out, 2.9 Mbit/s at 750 ksym/s) with `echo.py`. radio-controller's
  simulator gained a splitter (TX into every RX), path loss and frequency
  error; the libiio backend finds the FMCOMMS5's RX DMA (`cf-ad9361-A`).
- Toolchain file for Zynq-7000 (`cmake/toolchains/zynq7000-armhf.cmake`).
- Deterministic time for tests: `Application::use_manual_clock()` /
  `advance_time()`, `AppRunner(args, {.manual_time = true})` + `r.advance(d)`,
  `Application::now()`.
- Threading: `background(work, then)`; `[forge].slow_task_ms` warns about
  long main loop tasks (`forge.loop.slow_tasks`); `web::Dashboard` refuses
  use outside the main loop; `call()` on the main loop runs at once.
  [docs/threading.md](docs/threading.md).
- Tutorials ([docs/tutorials](docs/tutorials/README.md)) around a tested
  example (`examples/pump`); Doxygen API reference (`forgecpp_docs`,
  `docs/build-api.sh`).
- API policy ([docs/API.md](docs/API.md)) and `tools/api-check.py`:
  exported symbols and configuration keys of a minor series are kept.
- `FORGE_PCH` / `forge_add_app(... PCH)`: precompiled `<forge/forge.hpp>`
  for faster edit-compile cycles (off by default: clean builds of small
  projects get slower).
- Several instances of an application (`--instance`, `<name>@.service` with
  `forge_add_app(... TEMPLATE)`); forge-hub's proxy `/apps/<name>/`; card
  layouts per user with `[auth]`.
- Yocto: `yocto/` is a layer; `yocto/kas/qemuarm64.yml` builds and boots an
  image with runtime tests.
- CI: sanitizers (ASan+UBSan, TSan), the applications as real systemd units,
  an armhf cross build, forge.v1 compatibility, a nightly Yocto build.

### Changed

- cpp-httplib is built as a library (`libcpp-httplib`) instead of
  header-only: a file including `<forge/forge.hpp>` compiles in 8.1 s
  instead of 17.6 s (-O2; -O0: 5.8 instead of 9.7 s). pkg-config users link
  `-lcpp-httplib` (in `forgecpp.pc`).
- `zmq::Server` is a ROUTER socket with worker threads (REQ clients and
  messages unchanged); `serve_control(service, endpoint, workers)`.
- `ControlService`, `web::Dashboard`, forge-hub and `invoke_for` throw
  `forge::Error`: code that caught `http::HttpError` around control or
  dashboard calls catches `forge::Error` now.
- Declared configuration keys are filled in with their defaults after
  loading: `--print-config` and the `config` command show them.
- `forge::Application` is split into internal services (`src/app/`); the
  public API is unchanged.
- Python modules are installed into site-packages instead of
  `share/<name>/python`; `clients/forge_msg.py` is now `forge.v1.framing`.
- Yocto: `forgecpp-python` is now `python3-forge-v1` (RPROVIDES the old name).
- `sensor_service` is `sensor-service` (binary, configuration file); the
  example directories use the daemons' names (`examples/radio-controller`,
  `examples/spectrum-analyzer`, `examples/sensor-service`).
- Hub restart policies moved from forge-hub to the units.

### Deprecated

- The older JSON control commands (`{"cmd": ...}`) on `[forge].control`:
  counted in `forge.control.legacy_json` and named in a warning;
  `[forge].legacy_json = false` refuses them. Off by default in 0.4, removed
  in 0.5.

## 0.2.0

- ZeroMQ messaging, the AD9364 radio-controller daemon and its IQ streams,
  the AD936x simulator, channel roles from the configuration.
- The framework: `forge::Application` (main loop, timers, thread pool),
  configuration (TOML, schema, overrides), logging, HTTP server and client,
  web control panel, console, alarms, events; `install.sh`, the Yocto recipe,
  `forgecpp-new`.
