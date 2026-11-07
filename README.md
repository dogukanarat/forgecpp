# forgecpp

A **batteries-included** C++17 framework for Linux daemons and services.

Best-in-class libraries are bundled as git submodules, installed with a single
`install.sh`, and wired together into an application skeleton under the
`forge::` namespace. Every daemon you build shares the same foundation:
logging, configuration, CLI, thread pool, HTTP, **web UI**, **ZeroMQ**,
console, signal handling and systemd integration.

Applications are operated from web pages: you declare controls once in C++ and
forgecpp publishes them as a ready-made control panel, a JSON API, live updates
and `data-forge-*` attributes for your own HTML pages. The interactive console
(`-i`) is a backup channel to the same controls.

```cpp
#include <forge/forge.hpp>
using namespace std::chrono_literals;

class Pump : public forge::Application {
public:
    Pump() : Application({"pump", "1.0.0", "Pump control service"}) {}

protected:
    void setup_web(forge::web::Dashboard& ui) override {
        auto& g = ui.group("Pump");
        g.toggle("running", "Running", running_);
        g.slider("speed", "Speed", speed_, 0, 3000, 10).unit("rpm");
        g.value("pressure", "Pressure", pressure_).unit("bar").precision(2);
        g.button("flush", "Flush", [this] { flush(); }).confirm("Start flushing?");
    }
    void on_start() override {
        every(500ms, [this] { pressure_ = read_pressure(); });
    }

private:
    double read_pressure() { return running_ ? speed_ / 1000.0 : 0.0; }
    void flush() { logger()->info("flushing"); }
    bool running_ = false;
    int speed_ = 1200;
    double pressure_ = 0;
};

FORGE_MAIN(Pump)
```

```sh
./pump --http 8080                 # http://device:8080/forge/ -> control panel
./pump --http 8080 -i              # + backup console: controls / set speed 1500 / press flush
kill -HUP $(pidof pump)            # reload the configuration
```

## What's inside

| Library | Version | Purpose | In forgecpp |
|---|---|---|---|
| [spdlog](https://github.com/gabime/spdlog) | v1.17.0 | Fast, thread-safe logging | `forge::log`, `FORGE_INFO(...)` |
| [CLI11](https://github.com/CLIUtils/CLI11) | v2.7.2 | Command line parsing | `Application::cli()`, `setup_cli()` |
| [replxx](https://github.com/AmokHuginnsson/replxx) | master (1f149bf) | Console input, colors, tab completion | `forge::Console` |
| [BS::thread_pool](https://github.com/bshoshany/thread-pool) | v5.1.0 | Lightweight, fast thread pool | `forge::ThreadPool`, `Application::pool()` |
| [moodycamel::ConcurrentQueue](https://github.com/cameron314/concurrentqueue) | v1.0.5 | Lock-free inter-thread data transfer | `forge::Channel<T>` |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.12.0 | JSON reading/writing | `forge::json`, `save_json()` |
| [toml++](https://github.com/marzer/tomlplusplus) | v3.4.0 | TOML configuration | `forge::Config` |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | v0.65.0 | HTTP/HTTPS client and server | `forge::http::Server`, `forge::http::Client` |
| [doctest](https://github.com/doctest/doctest) | v2.5.3 | Unit testing | `forge/testing.hpp`, `forge_add_test()` |
| [libzmq](https://github.com/zeromq/libzmq) + [cppzmq](https://github.com/zeromq/cppzmq) | v4.3.5 / v4.11.0 | ZeroMQ messaging (pub/sub, push/pull, request/reply) | `forgecpp::zmq`, `forge/zmq.hpp` |
| [libiio](https://github.com/analogdevicesinc/libiio) | v0.26 | Industrial I/O devices (ADI transceivers, ADCs, ...) | `examples/radio_controller`, `examples/spectrum_analyzer` |

## Installation

Requirements: CMake ≥ 3.16, a C++17 compiler (GCC ≥ 9 / Clang ≥ 10), git,
libxml2 (for libiio; skip with `--without-iio`).
Optional: `ninja`, OpenSSL (for HTTPS), zlib (for compression), libusb-1.0 (`--iio-usb`).

```sh
git clone --recursive https://github.com/dogukanarat/forgecpp.git
cd forgecpp
./install.sh                         # installs into /usr/local (uses sudo if needed)
```

One-liner (clones into a temporary directory, installs and cleans up):

```sh
curl -fsSL https://raw.githubusercontent.com/dogukanarat/forgecpp/master/get.sh | bash
curl -fsSL https://raw.githubusercontent.com/dogukanarat/forgecpp/master/get.sh | bash -s -- --prefix /opt/forgecpp --ref v0.2.0
```

`install.sh` builds every dependency as its own CMake project and installs it
into the same prefix (each with its own `find_package` config), then builds
`libforgecpp` on top.

| Option | Description |
|---|---|
| `--prefix DIR` | Install location (default `/usr/local`) |
| `--static` | Static libraries (default: shared) |
| `--build-type T` | `Release`, `Debug`, `RelWithDebInfo`, `MinSizeRel` |
| `--tests` / `--examples` | Build and run the unit tests / build the examples |
| `--toolchain FILE` | Cross compile (Yocto SDK, OE `toolchain.cmake`) |
| `--destdir DIR` | Stage the install under `DIR` (packaging) |
| `--package [--arch A]` | Produce `forgecpp-<version>-<arch>.tar.gz` without touching the system |
| `--without-ssl`, `--with-ssl`, `--without-zlib` | cpp-httplib features |
| `--without-zmq`, `--without-iio`, `--iio-usb` | Skip ZeroMQ / libiio, or build libiio's USB backend |
| `--libdir DIR` | e.g. `lib64` instead of `lib` |
| `--skip-deps`, `--deps-only` | Only forgecpp / only the dependencies |
| `--uninstall` | Remove everything that was installed |

See `./install.sh --help` for the full list.

## Starting a new application

```sh
forgecpp-new my-daemon
cd my-daemon
cmake -B build && cmake --build build -j
ctest --test-dir build
./build/my-daemon -c config/my-daemon.toml      # http://localhost:8080/ and /forge/
./build/my-daemon -c config/my-daemon.toml -i   # + backup console
sudo cmake --install build     # binary (web/ embedded) + /etc/my-daemon/my-daemon.toml + systemd unit
```

Generated project:

```
my-daemon/
├── CMakeLists.txt              find_package(forgecpp) + forge_add_app() + forge_add_test()
├── src/main.cpp                forge::Application subclass (controls in setup_web)
├── web/index.html              the application's own page (embedded in the binary)
├── config/my-daemon.toml       default configuration
├── systemd/my-daemon.service.in  Type=notify, watchdog, reload
└── tests/test_my_daemon.cpp    doctest
```

In your own CMake project:

```cmake
find_package(forgecpp REQUIRED)
forge_add_app(mydaemon
    SOURCES src/main.cpp
    CONFIG  config/mydaemon.toml        # installed as <sysconfdir>/mydaemon/mydaemon.toml
    WEB     web                         # every file under web/ is embedded in the binary
    SERVICE mydaemon.service)           # installed under lib/systemd/system
# to embed pages into another target:
forge_add_web(mytool DIR web PREFIX /)
# or the classic way:
target_link_libraries(mytool PRIVATE forgecpp::forgecpp)
```

Without CMake: `pkg-config --cflags --libs forgecpp`.

## Framework tour

### `forge::Application`

`run(argc, argv)` does the following, in order:

1. Parses the standard flags and your own options with CLI11
2. Loads the TOML configuration and applies `-D key=value` overrides and flags
3. Sets up logging from the `[log]` section (console / rotating file / syslog)
4. Starts a thread pool with `[threads].workers` threads
5. Prepares the HTTP server (`[http]` or `--http`), the web UI (`[web]`) and the console (`-i`)
6. Calls `on_start()` and enters the **main loop**
7. `SIGINT`/`SIGTERM` → graceful shutdown, `SIGHUP` → `on_reload()`, a second signal → forced exit
8. Reports `READY=1`/`STOPPING=1` to systemd and feeds the watchdog when `WatchdogSec=` is set

All timers, tasks submitted with `post()`/`call()`, events published with
`emit()` and console commands run on the **main loop thread**. Application
state is therefore accessed without locks; hand heavy work to `pool()` and
bring the result back with `post()`:

```cpp
pool().detach_task([this] {
    auto result = heavy_work();
    post([this, result] { apply(result); });
});
```

| API | Description |
|---|---|
| `post(fn)` | Run `fn` on the main loop (from any thread) |
| `call(fn) -> std::future<R>` | Run on the main loop, get the result as a future |
| `invoke(fn) -> R` | Run on the main loop and wait for the result (runs inline if already on the main loop; use this in HTTP handlers) |
| `after(d, fn)`, `every(d, fn)`, `cancel(id)` | Timers (run on the main loop) |
| `emit(Event{...})`, `events().subscribe<Event>(...)` | Typed event bus |
| `config()` | Snapshot of the effective configuration |
| `alarms().raise(id, severity, message)`, `alarms().clear(id)` | Alarm management (from any thread) |
| `publish_web(event, json)`, `notify_web(message, level)` | Send an event / a short notification to open web pages |
| `logger()`, `pool()`, `http()`, `dashboard()`, `console()`, `cli()` | Services |
| `stop(code)`, `reload()` | Thread-safe lifecycle control |

Overridable hooks: `setup_cli`, `setup_http`, `setup_web`, `setup_console`,
`on_start`, `on_stop`, `on_reload`. If you prefer not to subclass, the same
hooks are available as lambdas: `hook_start`, `hook_stop`, `hook_reload`,
`hook_http`, `hook_web`, `hook_console` (see `examples/hello`).

#### Standard command line

```
-c,--config FILE        Configuration file (otherwise ./<name>.toml, <install>/etc/<name>/<name>.toml, /etc/<name>/<name>.toml are searched)
-D,--set KEY=VALUE      Override a config value (TOML value: 42, true, "x", [1,2]; anything else is a string)
-l,--log-level LEVEL    trace|debug|info|warn|error|critical|off
-v / -vv                debug / trace
--log-file FILE         Rotating log file
--syslog                Also log to syslog
-i,--interactive        Interactive console
--http [HOST:]PORT      Enable the HTTP server (includes GET /health)
--print-config          Print the effective configuration as JSON and exit
-V,--version
```

#### Configuration keys read by the framework

```toml
[log]
level = "info"
file = ""                # empty = disabled
max_size_mb = 10
max_files = 3
syslog = false
console = true
pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v"

[threads]
workers = 0              # 0 = number of CPUs

[http]
enabled = false
host = "0.0.0.0"
port = 8080
threads = 8              # each open browser tab holds one thread for live updates

[http.auth]              # when set, everything except /health requires credentials
user = ""                # Basic auth (the browser prompts for user/password)
password = ""
token = ""               # Authorization: Bearer <token> (for scripts)

[web]
enabled = true           # control panel under /forge/ when HTTP is enabled
title = "<name>"
root = ""                # if set, pages are served from disk (development, no rebuild)
refresh_ms = 500         # how often controls are checked for changes
max_clients = 8          # concurrently connected live pages
logs = true              # live log on the control panel
log_level = "info"
log_history = 200

[console]
enabled = false          # same as -i
prompt = "<name>> "
history = "~/.<name>_history"
```

### `forge::Config` (toml++ + nlohmann/json)

```cpp
auto cfg = forge::Config::load_file("/etc/app/app.toml");
int port   = cfg.get_or<int>("http.port", 8080);            // default when missing
auto name  = cfg.require<std::string>("device.name");       // ConfigError when missing
auto hosts = cfg.get_or<std::vector<std::string>>("peers", {});
auto first = cfg.get<std::string>("servers[0].host");       // std::optional
auto db    = cfg.section("database");
cfg.apply_override("log.level=debug");
forge::json j = cfg.to_json();
```

Type mismatches throw `ConfigError` with file:line:column information.

### `forge::log` (spdlog)

```cpp
forge::log::info("connected to {}:{}", host, port);
auto net = forge::log::get("net");       // named logger sharing the same sinks
net->debug("packet {}", id);
FORGE_WARN("with file/line info");        // compiled out below SPDLOG_ACTIVE_LEVEL
forge::log::set_level("debug");           // at runtime
```

### `forge::Channel<T>` (moodycamel::ConcurrentQueue)

```cpp
forge::Channel<Frame> frames;
frames.push(Frame{...});                   // producer thread
while (auto f = frames.pop()) handle(*f);  // consumer; ends after close() once drained
auto f = frames.try_pop();                 // non-blocking
auto g = frames.pop_for(100ms);            // with timeout
```

### `forge::ThreadPool` (BS::thread_pool)

```cpp
auto fut = pool().submit_task([] { return compute(); });
pool().detach_loop(0, n, [&](std::size_t i) { process(i); });
pool().wait();
```

### `forge::http` (cpp-httplib + nlohmann/json)

```cpp
server.get_json("/api/items/:id", [](const forge::http::Request& req) {
    return forge::json{{"id", req.path_params.at("id")}};
});
server.post_json("/api/items", [](const auto& req, const forge::json& body) {
    if (!body.contains("name")) throw forge::http::HttpError(422, "name is required");
    return forge::json{{"created", true}};
});

forge::http::Client client("http://10.0.0.5:8080");
client.timeout(2s).bearer_token(token);
forge::json items = client.get_json("/api/items");   // >= 400 -> HttpError
```

An invalid JSON body is answered with 400, an uncaught exception with 500 and
an unknown path with 404, each with a JSON error object.

### Web UI (`forge::web`)

Daemons are controlled from web pages. There are three layers, all built on
the same control declarations:

**1. Control panel — no HTML required.** Declare controls in `setup_web()` and
`http://device:8080/forge/` becomes a ready-made panel (groups, buttons,
switches, sliders, numbers, text, drop-downs, read-only values, live log,
connection status, mobile layout, light/dark theme):

```cpp
void setup_web(forge::web::Dashboard& ui) override {
    ui.title("Pump station 3");
    ui.link("Schematic", "/schema.html");             // link to your own pages

    auto& g = ui.group("Motor");
    g.button("start", "Start", [this] { start(); }).primary();
    g.button("stop", "Stop", [this] { stop_motor(); }).confirm("Stop the motor?").danger();
    g.toggle("enabled", "Enabled", enabled_);                       // bool&
    g.slider("speed", "Speed", speed_, 0, 3000, 10).unit("rpm");    // int&/double&
    g.number("limit", "Limit", limit_).range(0, 100).unit("%");
    g.select("mode", "Mode", {"auto", "manual"}, mode_);            // std::string&
    g.text("label", "Label", label_);
    g.value("temp", "Temperature", temp_).unit("°C").precision(1);  // read-only
    g.value("load", "Load", [this] { return pump_.load(); });       // computed value
    // getter/setter instead of a variable: g.toggle("x", "X", [this]{...}, [this](bool v){...});
}
```

- Bound variables are read and written **on the main loop**; no locking needed.
- Values are validated server-side (type, range, options, read-only); invalid
  requests get 400/403/404 and never reach the variable.
- Changes are detected every `[web].refresh_ms` and pushed to all open pages
  immediately via Server-Sent Events. Every web/console action is logged (who, what).

**2. Your own pages — just HTML.** Files in your `web/` directory are embedded
in the binary with `forge_add_app(... WEB web)` (no separate files to deploy)
and served under `/`. `forge.js` binds them to the same controls with attributes:

```html
<link rel="stylesheet" href="/forge/forge.css">   <!-- ready-made styles (optional) -->
<script src="/forge/forge.js"></script>

<span class="forge-dot" data-forge-connection></span>                  <!-- connection -->
<b data-forge-value="temp" data-forge-precision="1"></b> °C            <!-- live value -->
<input type="range" min="0" max="3000" data-forge-bind="speed">        <!-- two-way -->
<input type="checkbox" data-forge-bind="enabled">
<select data-forge-bind="mode"><option>auto</option><option>manual</option></select>
<input type="radio" name="m" value="auto" data-forge-bind="mode">     <!-- one radio per option -->
<button data-forge-press="stop" data-forge-confirm="Are you sure?">Stop</button>
<div data-forge-show="enabled">only visible while enabled</div>
<div class="forge-alarms-host" data-forge-alarms></div>               <!-- alarm bell + banner + panel -->
```

From JavaScript: `forge.on('state', changed => ...)`, `await forge.set('speed', 1200)`,
`await forge.press('start')`, `forge.on('alarm', d => ...)` (server side:
`publish_web("alarm", {...})`), `forge.toast(...)`, `await forge.api('/api/...')`,
`forge.alarms`, `forge.on('alarms', list => ...)`, `forge.ack(id)`, `forge.ackAll()`.
While developing, `-D web.root=web` serves the pages from disk: save, reload the browser.

**3. JSON API — for scripts and other systems:**

| Path | Description |
|---|---|
| `GET /forge/api/info` | Name, version, hostname, uptime |
| `GET /forge/api/ui` | Panel description + values |
| `GET /forge/api/state` | `{"id": value, ...}` |
| `GET/POST /forge/api/controls/<id>` | Read / write with `{"value": ...}`; empty body for buttons |
| `GET /forge/api/alarms` | Alarm list (active + history) |
| `POST /forge/api/alarms/<id>/ack`, `POST /forge/api/alarms/ack` | Acknowledge one / all alarms |
| `GET /forge/api/events` | SSE: `state`, `alarms`, `log`, `app` events |
| `GET /forge/api/logs` | Recent log lines |
| `GET /health` | Status + alarm summary (for monitoring; no auth required) |

```sh
curl -u admin:secret -X POST http://device:8080/forge/api/controls/speed -d '{"value": 1500}'
```

**Security:** `[http.auth]` enables Basic auth and/or a Bearer token
(everything is protected, pages included, except `/health`). A warning is
logged at startup when a network-facing device has no auth configured. For
HTTPS, put the application behind a reverse proxy or use cpp-httplib's
`SSLServer` through `native()`.

**The console is the backup:** in the console opened with `-i`, the same
controls are available through `controls`, `set <id> <value>`, `press <id>`,
and alarms through `alarms` and `ack <id|all>` (with tab completion).

### ZeroMQ (`forgecpp::zmq`)

A separate library (link `forgecpp::zmq`, include `<forge/zmq.hpp>`), so
daemons that do not need it do not pull in libzmq. Messages are multipart:
**topic**, **JSON header**, optional **binary payload**.

```cpp
// fan-out (slow subscribers drop instead of stalling the others)
forge::zmq::Sender pub(forge::zmq::Sender::Kind::pub, "tcp://*:5555");
pub.send("iq", {{"seq", seq}, {"samples", n}}, samples.data(), bytes);
pub.subscriptions();                                  // who is listening?

// receive on a background thread
forge::zmq::Receiver::Options opts;
opts.topics = {"iq"};
forge::zmq::Receiver sub(forge::zmq::Receiver::Kind::sub, "tcp://radio:5555",
                         [this](forge::zmq::Message&& m) { post([this, m] { handle(m); }); }, opts);

// lossless work distribution: Sender::Kind::push / Receiver::Kind::pull

// JSON request/reply
forge::zmq::Server server("tcp://*:5600", [](const forge::json& req) { return forge::json{{"pong", true}}; });
forge::zmq::Client client("tcp://host:5600", 2s);     // timeout + automatic reconnect
auto reply = client.call("ping");                     // throws forge::zmq::Error on failure
```

**Controls over ZeroMQ:** `forge::zmq::serve_application(app, "tcp://*:5557")`
(call it in `on_start()`) exposes the same controls and alarms as the web UI
and the console, with the same validation:
`{"cmd":"set","id":"speed","value":1500}`, `get`, `state`, `ui`, `press`,
`alarms`, `ack`, `info`, `ping`. The controls exist even when HTTP is
disabled, so a headless daemon can be operated over ZeroMQ alone.
`examples/radio_controller/clients/radioctl.py` is a ready-made command line client.

### Alarms (`forge::Alarms`)

A persistent, HMI/SCADA-style alarm list for conditions that need operator
attention. Unlike short-lived notifications (`notify_web`), an alarm stays
visible until an operator acknowledges it:

```cpp
// callable from any thread; raising the same id again does not create a new entry:
// message/time are updated and a counter increases (x12) - no flood of messages
alarms().raise("tank.level", forge::Severity::warning, fmt::format("Tank {:.0f} % full", level));
alarms().clear("tank.level");                                   // condition is gone
alarms().set("sensor.fault", fault, forge::Severity::error, "Sensor not responding");  // by condition
```

- **Lifecycle:** *active + unacknowledged* → (operator acknowledges) →
  *active + acknowledged* → (condition clears) → *history*. An alarm escalated to
  a higher severity needs a new acknowledgement. An alarm that clears and comes
  back before being acknowledged (flapping) keeps one entry and increases its counter.
- **Web:** a bell in the header with a count badge colored by severity; a banner
  under the header while unacknowledged alarms exist (most important alarm,
  "+N more", View / Acknowledge); a side panel with Active / History tabs,
  times, counters and who acknowledged. Your own pages get the same component
  with `<div data-forge-alarms></div>`.
- **Severities:** `info`, `warning`, `error`, `critical` (the critical banner is solid red).
- Every raise, acknowledgement and clear is logged; `/health` returns an alarm summary.

### `forge::Console` (replxx)

Enabled with `-i`; it is the backup control channel when the web UI is not
reachable. History, tab completion, hints and highlighting are included; while
the console is open, log lines are printed above the prompt without corrupting
it. Built-in commands: `help`, `status`, `log-level`, `config`, `reload`,
`controls`, `set`, `press`, `alarms`, `ack`, `history`, `clear`, `exit`.

```cpp
void setup_console(forge::Console& c) override {
    c.add("rate", "rate [hz]  show or change the sampling rate",
          [this, &c](const forge::Console::Args& a) {
              if (!a.empty()) rate_ = std::stod(a[0]);
              c.print("rate: {} Hz", rate_);
          },
          [](const forge::Console::Args&) { return std::vector<std::string>{"1", "10", "100"}; });
}
```

### Testing

```cpp
#include <forge/testing.hpp>     // doctest + TempDir + wait_until

TEST_CASE("config defaults") {
    auto cfg = forge::Config::parse("port = 80");
    CHECK(cfg.get_or<int>("port", 0) == 80);
}
```

```cmake
forge_add_test(mydaemon_tests SOURCES tests/test_main.cpp)   # main() is provided
```

## Yocto / embedded deployment

forgecpp is meant to run on embedded Linux. The whole stack cross-compiles with
a Yocto-style toolchain; on aarch64 the full test suite passes and the example
daemons run with their web UI. Stripped sizes on aarch64: `libforgecpp.so`
≈ 2.1 MB, `libspdlog.so` ≈ 650 KB, `libreplxx.so` ≈ 330 KB; a scaffolded
application with its embedded web page ≈ 580 KB. Runtime dependencies are only
libstdc++/libc (plus OpenSSL/zlib when enabled).

- **With bitbake:** `yocto/recipes-support/forgecpp/forgecpp_git.bb` fetches
  the submodules with `gitsm://` and runs `install.sh` with the Yocto
  toolchain. Applications then only need `DEPENDS = "forgecpp"` and
  `inherit cmake systemd`; see [yocto/README.md](yocto/README.md).
- **Without bitbake:** with an SDK toolchain,
  `./install.sh --package --prefix /usr --toolchain ...` produces a tar.gz;
  on the target run `tar -xzf forgecpp-*.tar.gz -C /`.

Web pages are compiled into the binaries, so read-only root filesystems work
without extra files.

## Examples

| Example | What it shows |
|---|---|
| `examples/hello` | The smallest daemon (lambda style), a console command |
| `examples/sensor_service` | Every building block: channel, thread pool, events, control panel, own page, alarm with hysteresis |
| `examples/pipeline` | **Multi-threaded application:** N producer threads → lock-free `Channel` → dispatcher thread → `ThreadPool` workers → result channel → main loop. Bounded in-flight work (backpressure), atomic counters, alarms raised from worker threads, clean shutdown |
| [`examples/radio_controller`](examples/radio_controller/README.md) | **AD9364 radio daemon (libiio + ZeroMQ):** controls the transceiver (LO, sample rate, bandwidth, AGC/gain, TX attenuation) through libiio and streams IQ samples over ZeroMQ (RX via PUB, TX via PULL, JSON control via REP). Live spectrum and waterfall page, alarms for overflows, underflows, temperature and a lost device (with automatic reconnect), state persistence, a hardware simulator, Python clients, systemd unit and its own Yocto recipe. Built when forgecpp has ZeroMQ |
| [`examples/spectrum_analyzer`](examples/spectrum_analyzer/README.md) | **AD9364 spectrum analyzer with a web UI:** realtime (one capture) or swept (LO stepping) spans from 10 kHz to 70 MHz - 6 GHz, RBW/window/video averaging, peak/sample/average/negative-peak detectors, three traces (clear/write, max hold, min hold, average, view), four markers with delta, peak search/next/left/right, peak track and signal track, waterfall, channel power and occupied bandwidth, limit line alarm, CSV/PNG/IQ export, spectrum stream over ZeroMQ. Shares the AD936x backend with the radio controller |
| `examples/thermostat` | **Web-first application:** its own multi-page site (overview, live SVG chart, settings form), custom REST API with per-field validation, live data via `publish_web`, radio/segmented controls, alarms, settings persisted with `save_json` |

```sh
./install.sh --prefix ~/.local --examples
cd build/forgecpp/examples
./pipeline -c pipeline.toml --producers 4 -i      # http://localhost:8080/forge/
./thermostat -c thermostat.toml                    # http://localhost:8080/
./sensor_service -c sensor_service.toml -i --simulate-spikes
./radio_controller/radio-controller -c radio_controller/radio-controller.toml   # simulated AD9364
./radio_controller/radio-controller --backend iio --uri ip:192.168.2.1         # real AD936x via iiod
./spectrum_analyzer/spectrum-analyzer -c spectrum_analyzer/spectrum-analyzer.toml  # http://localhost:8080/
```

The radio controller and the spectrum analyzer are also standalone CMake
projects (`cmake -S examples/radio_controller -B build
-DCMAKE_PREFIX_PATH=<forgecpp prefix>`) that install the daemon, its config
and systemd unit; their Yocto recipes in `yocto/recipes-connectivity` build
them that way. Both use `examples/common/ad936x` (libiio backend, simulator,
FFT/windows).

## Updating dependencies

```sh
git -C third_party/spdlog fetch --tags && git -C third_party/spdlog checkout v1.x.y
git add third_party/spdlog && git commit -m "spdlog: v1.x.y"
```

## Directory layout

```
include/forge/     public headers (forge.hpp includes everything)
src/               libforgecpp sources
src/web/           built-in control panel, forge.js, forge.css (embedded in the library)
third_party/       submodules
cmake/             package config, pkg-config, forge_add_app/forge_add_web/forge_add_test
tools/forgecpp-new project scaffolder
examples/          hello, sensor_service, pipeline (multi-threaded), thermostat (web site),
                   radio_controller (AD9364 over libiio + ZeroMQ),
                   spectrum_analyzer (AD9364 spectrum analyzer), common/ad936x
tests/             doctest unit tests
yocto/             bitbake recipes (forgecpp, radio-controller, spectrum-analyzer)
install.sh, get.sh installation
```

## License

MIT (see [LICENSE](LICENSE)). The bundled libraries come with their own
licenses (MIT, BSD-2/3-Clause, BSL-1.0; libzmq MPL-2.0; libiio LGPL-2.1-or-later
with GPL-2.0-or-later command line tools); see `third_party/*/LICENSE*` and
`third_party/libiio/COPYING*.txt`. Link libzmq and libiio dynamically (the
default) to keep the LGPL/MPL obligations simple.
