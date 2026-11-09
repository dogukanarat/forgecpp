# Changelog

forgecpp follows [semantic versioning](https://semver.org). Until 1.0 a minor
version may break the C++ API: the library's SOVERSION is `<major>.<minor>`
and `find_package(forgecpp 0.4)` only accepts 0.4.x. The messages
(`proto/forge`) only change compatibly; CI compares them with the API of
the last release (`proto/forge/API.json`, `tools/proto-compat.py`).

## Unreleased (0.4.0)

### Added

- **c2** (`forge/c2.hpp`): the command-and-control message of zephyrus, byte for
  byte: the 8-byte header (version, flags, command number, sequence number,
  length), the flags, and the NACK status. forgecpp and zephyrus devices can
  now exchange messages directly. The codec does not allocate and does not
  throw.
- **forge::Status** (`forge/status.hpp`): the int32 status of zephyrus (bit 31,
  module id, error code). Error codes on the wire use the errno numbers of
  Zephyr; Status converts them to the numbers of the host (ENOSYS is 38 on
  Linux and 88 on Zephyr). `http_status()` gives the HTTP answer for the web.
- Wire compatibility tests: `-DFORGE_ZEPHYRUS_DIR=<zephyrus>` builds the C code
  of zephyrus on the host and compares it with forgecpp on the same input.
- **Serial link** (`forge/serial.hpp`): c2 frames on a UART or USB CDC line,
  framed as zephyrus's `modules/lib/cobs` (COBS, CRC16-CCITT, fragments of at
  most 255 per message). `serial::Link` works both ways: `call()` to the other
  end, `on_request` for its requests, `push()` and `on_push` for reports.
  `[forge].serial`, `serial_baud`, `serial_frame_size` answer the
  application's commands on a tty, as `[forge].control` does on ZeroMQ. The
  compatibility tests compare the bytes and the decisions on damaged streams
  with zephyrus's C code. Python: `forge.serial.Client` (standard library only).
- radio-controller: typed commands `tune` and `status` (`radio.proto`).
- `docs/CODING_STANDARDS.md`: the rules of forgecpp, from the principles of
  zephyrus, with the naming of forgecpp.
- `tools/check-layers.py` (test `forgecpp_layers`): each module uses only
  modules of its own level or lower (core, services, link, front ends,
  application).
- **Commands (tmtc)** (`forge/tmtc.hpp`): every command is a request and a
  reply with a number, in a `tmtc::Registry`. `tmtc::Dispatcher` answers c2
  frames (reply, ACK, NACK with the status, replies in parts with the "more"
  flag); the same handler answers JSON (`POST /forge/api/tmtc/<name or
  number>`, list: `GET /forge/api/tmtc`). `Application::command<Req, Rsp>()`
  adds one that runs on the main loop. forgecpp's own commands use numbers
  0x0001..0x00FF (`proto/forge/messages.proto`); applications start at 0x0100.
- **nanopb messages** (`forge/pb.hpp`, `third_party/nanopb`): protobuf
  messages as C structs with a bound on every string, bytes and repeated
  field, so no message allocates. `forge_add_proto()` generates them (and
  `<stem>.forge.hpp` with the names for JSON); a field without a bound, a
  map or a `google.protobuf` type is refused at build time. JSON: every field
  written, 64-bit numbers as strings, enums by name, bytes as base64.
- `forge::Subscription`, `forge::Listeners<Args...>` (`forge/subscription.hpp`):
  a listener is removed when its Subscription goes (also while it runs on
  another thread: the destructor waits). `log::add_listener()`,
  `Alarms::on_change()`, `Audit::on_record()` and `Health::on_change()`
  return one.
- `forge::Remote` (`forge/remote.hpp`): get / set / press / call on another
  program's command link (what spectrum-analyzer, qam-modem and
  channel-emulator use to drive radio-controller).
- `zmq::answer(registry, request)` serves a registry on a `zmq::Server`;
  `zmq::make_message()` builds a stream message with the standard header.
- Python: `forge.c2` (commands over the link, with parts and NACKs) and
  `forge.framing` (streams), with `forge.messages_pb2` and `forge.hub_pb2`.
- `forge::base64` (`forge/base64.hpp`), shared by auth, HTTP and the JSON of
  bytes fields.
- `tools/check.sh`: on this machine, what CI checks (format, layers, build,
  tests, fuzz, API and message records); `--asan`, `--tsan PREFIX`,
  `--tidy`, `--fix`. `tools/check.sh --install-hook` adds a quick pre-commit
  check (`tools/git-hooks/pre-commit`).
- `.clang-format`, `.clang-tidy`, `.editorconfig`; `CMakePresets.json`
  (dev, release, asan, tsan, fuzz, armhf; libraries from `FORGECPP_DEPS`).
- Panel: a **Commands** page (every command with a form built from its
  request message, and a JSON editor with colours, live validation that says
  the line and column of a mistake, Format, Tab and Ctrl+Enter; the reply
  coloured the same way, with its time and a Copy button) and a **Watch** button per stream on
  the Streams page (its messages as they go out, at most 5 a second, without
  the blob). HTTP: `GET /forge/api/tmtc/<name>` (the request and reply
  schemas), `GET /forge/api/streams/<name>/watch` (server-sent events).
- Docs: `docs/protocol.md` (the c2 frame, flags, answers, the status and
  its errno numbers: enough to write a client or a device) and
  `docs/commands.md` (adding a command, step by step).
- `forgectl tmtc [name ['{json}']]` lists or runs any command;
  `forgectl stream <name> [-n N]` prints a stream's messages.
- `pb::schema(type)`: a message's fields as JSON (kind, bits, bounds, enum
  names, nested messages). `zmq::Stream::watch()`: see a stream's messages
  from inside the program; costs one atomic read while nobody watches.
- Browser tests of the panel (`tests/web`, Playwright): controls, buttons,
  alarms, the Commands page and the stream inspector, against a small test
  application. ctest runs them when node and Playwright are installed.
- Fuzz targets for c2 frames, the dispatcher, protobuf bytes and JSON
  (`tests/fuzz`): a short ctest by default, libFuzzer with
  `-DFORGECPP_FUZZ=ON`.

- `forge::Worker` (`forge/worker.hpp`): a named thread on `std::jthread` that
  stops when asked; `forge::sleep_for(stop, d)` returns at once on stop.
  `Application::worker(name, body)` applies `[threads.<name>]`, raises the
  alarm `worker.<name>` when the body throws, and stops the thread before
  `on_stop()`. sensor-service uses it.
- `background(work, then, on_error)`: the error of `work` on the main loop.
- `zmq::Sender::send_wait(message, timeout)`: waits for room instead of
  dropping (backpressure); channel-emulator uses it.
- `Group::value(id, label, ...)` takes a `std::atomic<T>`, a
  `metrics::Counter` or a `metrics::Gauge` directly.
- Generated systemd units are hardened: `NoNewPrivileges`, `PrivateTmp`,
  `ProtectSystem=full`, `ProtectHome=read-only`, `ProtectControlGroups`,
  `RestrictSUIDSGID`, `LockPersonality` (devices, `/sys` and real-time
  priorities stay allowed); `forge_add_app(... NO_HARDENING)` leaves them out.
- `.github/dependabot.yml`: monthly updates of the actions and the bundled
  libraries (not protobuf, libzmq, libiio).
- `forge::Error::module()`: the module a refusal goes out with on a link.
- Panel: the log card filters by level and by text ("12 of 300").
- `[web].live_reload` (on): with `web.root`, a saved file reloads the open
  pages of the application (not `/forge/`).
- `forgecpp-new --minimal [--with-command] [--with-stream]`.
- `tools/release.sh <version>`: checks, records the APIs, dates the
  CHANGELOG, commits and tags (no push).
- `forgecpp_bench`: protobuf, c2 dispatch, serial framing, ZeroMQ command and
  stream rates.
- `get` and `set` carry the value in a typed member too (`number`, `flag`,
  `text`; `oneof typed`), so a microcontroller needs no JSON parser.

### Changed

- A refusal by an application command's handler (a `forge::Error` it throws)
  goes out on a link with module **app** (64) instead of tmtc; forgecpp's own
  commands and forgecpp's checks (role, busy main loop) stay tmtc.
- Python `c2.Client.call_parts()` no longer drops the parts before an ACK
  that ends a list; `import forge.c2` no longer needs pyzmq until a
  `c2.Client` is made.
- `zmq::serve_control()` and `zmq::serve_application()` are declared in
  `forge/control.hpp` (was `forge/zmq.hpp`): the transport no longer depends
  on the application. Code that includes `forge/forge.hpp` needs no change.
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
    `log::short_name()`). JSON lines and the `level` of log messages keep the
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
    `log::Record`). Listeners run on the logging thread: call
    `log::flush()` before relying on a record having arrived.
  - The logging thread blocks asynchronous signals (SIGTERM, SIGINT, ...), so
    they reach the application's signal handling even when logging starts
    first.
- Version 0.4.0: `libforgecpp.so.0.4`, `find_package(forgecpp 0.4)`; the C++ API
  record is `api/forgecpp-0.4.json`.
- **forge.v1 is gone.** The command endpoint (`[forge].control`), the events,
  the streams' header, forge-hub's registry and forgectl use c2 frames and
  the nanopb messages of `proto/forge` instead of Google protobuf.
  libforgecpp no longer links libprotobuf; protoc only runs on the build
  machine (install.sh builds it for the host and installs it as
  `share/forgecpp/protoc`), with Python's protobuf module for the generator.
  - Streams: `stream<T>(name[, options])` instead of `stream(name,
    "pkg.v1.Type")`; `Message::header` is a `forge_stream_header`;
    `Message::as<T>()` takes a nanopb struct. The examples' messages are now
    `radio.iq_block`, `radio.tx_block`, `sa.spectrum` (its levels are the
    blob, float32) and `pump.reading`.
  - `zmq::Client` sends c2 commands (`call()`, `call_parts()`, `request()`)
    and returns a `Status`; `get()` / `set()` / `press()` / `call_command()` /
    `request_json()` moved to `forge::Remote`. `zmq::Server` handlers return
    the frames of the answer (`std::vector<std::string>`); `ServerOptions::busy`
    answers requests that waited too long.
  - The older JSON commands on the control endpoint (`{"cmd": ...}`),
    `[forge].legacy_json`, `/forge/api/rpc`, `forge/proto.hpp`,
    `zmq::serve_control()` and `ControlService::handle()` / `schema()` are
    removed (the "commands" and "controls" commands list the same).
  - Logs, audit records, health, alarms and metrics are plain C++ structs
    (`log::Record`, `Audit::Record`, `Health::Report`, `metrics::Snapshot`)
    with `to_json()`. `log::remove_listener()` is gone (reset the
    Subscription).
  - A refusal on the link is a NACK with the status (module and errno, in
    Zephyr's numbering); only HTTP carries the text of the reason. c2 frames
    carry no token: commands on a link get `[auth].default_role`.
  - Python: the `forge` package replaces `forge-v1` (`from forge import
    framing`); Yocto: `python3-forge` replaces `python3-forge-v1` (it
    RPROVIDES the old name) and there is no `forgecpp-protoc` package any
    more.
  - `Application(info, installed_config)` and `zmq::Receiver(..., options)`
  take `const std::string&` / `const Options&` (were by value).
- `Application::command()` takes a role (default operator): the caller
  needs it, and every call is written to the audit trail with its request.
  `forge::Role::viewer` marks a query (no check, no record). Before, the
  program's own commands were neither checked nor recorded.
- `forge::testing::AppRunner` reports at once when the application ends
  before it starts (it waited 20 s, then terminated the test program).
- `Application::command()` throws when it is called after the links
    started: add commands in the constructor or `setup_web()`.
- `install.sh --profile embedded` keeps the libraries shared (it built them
  static): the applications of a device share one `libforgecpp.so`,
  `libzmq.so`, `libcpp-httplib.so` and `libiio.so`. `--static` still builds
  static libraries.
- CI: a push builds and tests once (g++); the other compilers, sanitizers,
  the armhf build, the systemd job and the API reference run on pull
  requests, weekly and by hand.

### Fixed

- `ENAMETOOLONG` (a text longer than its field) answers HTTP 400 instead of 500.
- `pb::Message` no longer compares an object's address with null in a
  concept: GCC with `-fsanitize=undefined` (or `-fno-delete-null-pointer-checks`)
  refused it and no command compiled. forge-proto.py also writes
  `pb::is_message<T>`; regenerate with the new forge-proto.py.
- `tools/check.sh` reports a failed build instead of stopping with SIGPIPE.
- A program whose `[forge].data` or `[forge].events` port is already taken
  stops with "Address already in use" instead of aborting (`zmq::Publisher`
  threw with its thread still running).

- A log listener is removed only once it is no longer running: the web
  panel's log view and the log events could be destroyed while the logging
  thread was still calling them.
- A telnet client that stops reading is disconnected after 5 s instead of
  blocking whoever writes to it (the logging thread with `logs on`).
- `zmq::set_source()` and the stream headers raced when two applications ran
  in one process.

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
