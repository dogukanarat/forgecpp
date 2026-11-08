# Changelog

forgecpp follows [semantic versioning](https://semver.org). Until 1.0 a minor
version may break the C++ API: the library's SOVERSION is `<major>.<minor>`
and `find_package(forgecpp 0.4)` only accepts 0.4.x. The forge.v1 protocol
(`proto/forge/v1`) only changes compatibly; CI compares it with the API of
the last release (`proto/forge/v1/API.json`, `tools/proto-compat.py`).

## Unreleased (0.4.0)

### Changed

- **C++20** is required (was C++17): the library, its exported CMake target
  (`cxx_std_20`), `forge_add_app()`, pkg-config (`-std=c++20`) and the
  examples. Supported toolchains: GCC ≥ 13 or Clang ≥ 17 with libstdc++ 13
  (Yocto scarthgap, Ubuntu 24.04, the armhf cross compiler). Applications
  built against forgecpp must compile as C++20.
- `std::format` instead of `fmt::format` everywhere (forgecpp no longer
  re-exports fmt). Applications that used `fmt::format` through forgecpp's
  headers include `<format>` and use `std::format`.
- **Console: linenoise instead of replxx** (unmaintained since 2021).
  linenoise is compiled into libforgecpp (no separate library). History, tab
  completion and hints stay (the hint now also shows a command's arguments
  from its help text); colouring of the typed command is gone. Output from
  other threads and log lines are printed above the line being typed. Piped
  input is read line by line and `stop()` interrupts it (it used to detach the
  thread). A terminal without a window size no longer makes the console wait
  for a cursor-position answer. `Console::Options` gained `input_fd` /
  `output_fd`.
- **Logging: Quill instead of spdlog**, behind forgecpp's own facade
  (`forge/log.hpp`; Quill is header-only, `libspdlog.so` is gone). A log call
  formats with `std::format` on the calling thread (the format string is
  checked at compile time) and queues the text; a background thread
  (`forge-log`) writes the terminal, the file, syslog, the history and the
  listeners, so a log call never waits for a terminal or a disk.
  - `forge::Logger` (`forge::log::Logger`) replaces
    `std::shared_ptr<spdlog::logger>`: `logger()->info(...)` keeps working,
    `logger().info(...)` too. Levels are `forge::log::Level` (`warn` is shown
    as `warning`).
  - `log.pattern` uses named fields instead of spdlog's flags:
    `"[{time}] [{level}] [{logger}] {message}"`, plus `{thread}`, `{file}`,
    `{line}`. Configuration files with a spdlog pattern (`%v`, ...) must be
    changed.
  - Levels are shown with three letters so the lines line up: `TRC DBG INF
    WRN ERR CRT` (terminal, file, telnet, forgectl, web panel;
    `log::short_name()`). JSON lines and forge.v1 `LogRecord.level` keep the
    full names (`info`, `warning`, ...).
  - Colours: level colours on the console, in telnet (`color on|off`), in
    `forgectl logs` / `watch` and in the web panel's log view. New key
    `log.color` = `auto` (a terminal, `NO_COLOR` unset, not journald) |
    `always` | `never`. Under journald each line carries its syslog priority
    (`<4>`) instead.
  - `FORGE_RT_TRACE..CRITICAL(logger, fmt, ...)` (`forge/log_rt.hpp`) for
    real-time threads: the arguments are copied and formatted on the
    background thread, the queue drops instead of blocking when full.
  - `FORGE_INFO(...)` etc. log with the caller's file and line; they are no
    longer compiled out by a compile-time level.
  - `log::set_terminal_writer(fn)` replaces `set_console_sink()`;
    `add_sink()` / `remove_sink()` are gone, use `add_listener()` (records as
    `forge.v1.LogRecord`). Listeners run on the logging thread: call
    `log::flush()` before relying on a record having arrived.
  - The logging thread blocks asynchronous signals (SIGTERM, SIGINT, ...), so
    they reach the application's signal handling even when logging starts
    first.
- Version 0.4.0: `libforgecpp.so.0.4`, `find_package(forgecpp 0.4)`; the C++ API
  record is `api/forgecpp-0.4.json`.
- `forge::proto::source()` returns a copy (`std::string`) instead of a
  reference: several applications in one process (tests) set it.

### Fixed

- `log::remove_listener()` returns only once the listener is no longer
  running: the web panel's log view and the forge.v1 log events could be
  destroyed while the logging thread was still calling them.
- A telnet client that stops reading is disconnected after 5 s instead of
  blocking whoever writes to it (the logging thread with `logs on`).
- `proto::set_source()` / the forge.v1 headers raced when two applications
  ran in one process.

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
  UDP in and out, 2.9 Mbit/s at 750 ksym/s) with `echo.py`; channel-emulator
  (noise, frequency offset/Doppler, phase noise, clock error, multipath,
  fading, IQ imbalance, DC, interference in the TX path; `common/channel`).
  qam-modem's receiver has a decision feedback equalizer (least squares on
  the preamble and header, LMS through the packet) and gain tracking.
  radio-controller drops streaming TX blocks older than `tx_max_age_ms`, so
  the TX latency cannot build up after stalls (qam-modem sets it). radio-controller's
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
