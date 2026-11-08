# forgecpp

A **batteries-included** C++20 framework for Linux daemons and services.

Best-in-class libraries are bundled as git submodules, installed with a single
`install.sh`, and wired together into an application skeleton under the
`forge::` namespace. Every daemon you build shares the same foundation:
logging, configuration, CLI, thread pool, HTTP, **web UI**, **ZeroMQ**,
console, signal handling and systemd integration.

Every application is operated, observed and connected **the same way**, so an
engineer moving between applications finds the same tools everywhere:

- **One message standard, forge.v1 (protobuf).** Controls, commands, alarms,
  logs, metrics, health, audit records and the application's own data streams
  travel as protobuf messages over ZeroMQ (or HTTP). Every message carries a
  header with its source, sequence number, time and trace context, so it can be
  followed from one daemon to the next and gaps are detected. A remote control
  panel receives these messages directly, in binary or as JSON.
- **The same ways in.** You declare controls and commands once in C++;
  forgecpp publishes them as a web control panel (`/forge/`), `data-forge-*`
  attributes for your own pages, the forge.v1 endpoint (`forgectl`), the local
  console (`-i`) and a **remote console over telnet**. All of them go through one
  control service with roles, leases and an audit trail.
- **Built-in observability.** Metrics (Prometheus `/metrics`), a flight
  recorder of trace spans (Perfetto), crash reports, structured (JSON) logs,
  health checks tied to the systemd watchdog.

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
./pump -D forge.control=tcp://*:5557 -D console.telnet=0.0.0.0:2323
forgectl -e tcp://device:5557 set speed 1500    # the same controls from anywhere
telnet device 2323                              # the console, remotely
kill -HUP $(pidof pump)            # reload the configuration
```

## What's inside

| Library | Version | Purpose | In forgecpp |
|---|---|---|---|
| [spdlog](https://github.com/gabime/spdlog) | v1.17.0 | Fast, thread-safe logging | `forge::log`, `FORGE_INFO(...)` |
| [CLI11](https://github.com/CLIUtils/CLI11) | v2.7.2 | Command line parsing | `Application::cli()`, `setup_cli()` |
| [linenoise](https://github.com/antirez/linenoise) | a473823 (2026-05) | Console line editing: history, tab completion, hints, output while typing (compiled into libforgecpp) | `forge::Console` |
| [BS::thread_pool](https://github.com/bshoshany/thread-pool) | v5.1.0 | Lightweight, fast thread pool | `forge::ThreadPool`, `Application::pool()` |
| [moodycamel::ConcurrentQueue](https://github.com/cameron314/concurrentqueue) | v1.0.5 | Lock-free inter-thread data transfer | `forge::Channel<T>` |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.12.0 | JSON reading/writing | `forge::json`, `save_json()` |
| [toml++](https://github.com/marzer/tomlplusplus) | v3.4.0 | TOML configuration | `forge::Config` |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | v0.65.0 | HTTP/HTTPS client and server | `forge::http::Server`, `forge::http::Client` |
| [doctest](https://github.com/doctest/doctest) | v2.5.3 | Unit testing | `forge/testing.hpp`, `forge_add_test()` |
| [libzmq](https://github.com/zeromq/libzmq) + [cppzmq](https://github.com/zeromq/cppzmq) | v4.3.5 / v4.11.0 | ZeroMQ messaging (pub/sub, push/pull, request/reply) | `forge/zmq.hpp`, forge.v1 endpoints |
| [protobuf](https://github.com/protocolbuffers/protobuf) | v21.12 | Message standard (binary + JSON mapping), `protoc` | `proto/forge/v1`, `forge_add_proto()`, `forgectl` |
| [libiio](https://github.com/analogdevicesinc/libiio) | v0.26 | Industrial I/O devices (ADI transceivers, ADCs, ...) | `examples/radio-controller`, `examples/spectrum-analyzer` |

## Installation

Requirements: CMake ≥ 3.16, a C++20 compiler (GCC ≥ 13 / Clang ≥ 17 with libstdc++ 13), git,
libxml2 (for libiio; skip with `--without-iio`). Python 3 with `pyzmq` and
`protobuf` for the Python clients (optional).
Optional: `ninja`, OpenSSL (for HTTPS), zlib (for compression), libusb-1.0 (`--iio-usb`).

```sh
git clone --recursive https://github.com/dogukanarat/forgecpp.git
cd forgecpp
./install.sh                         # installs into /usr/local (uses sudo if needed)
```

One-liner (clones into a temporary directory, installs and cleans up):

```sh
curl -fsSL https://raw.githubusercontent.com/dogukanarat/forgecpp/master/get.sh | bash
curl -fsSL https://raw.githubusercontent.com/dogukanarat/forgecpp/master/get.sh | bash -s -- --prefix /opt/forgecpp --ref v0.3.0
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
| `--without-iio`, `--iio-usb` | Skip libiio, or build libiio's USB backend |
| `--system-protobuf`, `--protoc PATH` | Use the system's protobuf; host `protoc` for cross builds (default: built from the bundled sources with `HOST_CC`/`HOST_CXX`) |
| `--profile embedded` | MinSizeRel, LTO, static libraries, no tests (small targets) |
| `--sanitize LIST` | `address,undefined` or `thread`, applied to the dependencies too (use a separate prefix) |
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
├── CMakeLists.txt              find_package(forgecpp) + forge_add_app() (systemd unit with
│                               its restart policy) + forge_proto + forge_add_test()
├── src/my_daemon.hpp, main.cpp forge::Application subclass (schema, controls, commands, stream)
├── proto/my_daemon/v1/my_daemon.proto   its own messages (a "ticks" stream)
├── web/index.html              the application's own page (embedded in the binary)
├── config/my-daemon.toml       default configuration
├── tests/test_my_daemon.cpp    doctest + forge::testing::AppRunner
├── clients/watch.py            Python client of the stream (forge.v1 package)
└── README.md
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

New to forgecpp? The [tutorials](docs/tutorials/README.md) build a pump
controller step by step (its code is `examples/pump`, compiled and tested).
API reference: `docs/build-api.sh` (or `cmake --build build --target
forgecpp_docs`), then `build/docs/html/index.html`.

Threads in one sentence: the application's state belongs to its main loop;
other threads reach it with `invoke()` / `post()`, heavy work leaves it with
`background()`. Where each callback runs and which call to use:
[docs/threading.md](docs/threading.md).

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
| `invoke_for(timeout, fn) -> R` | Same, but gives up with `forge::Error(503, "main loop busy")` when the main loop has not started `fn` in time (`fn` then never runs). The framework's request handlers use it with `request_timeout()` (`[forge].request_timeout_ms`) |
| `call(fn) -> future` | Run on the main loop, get a future (runs at once when already on the main loop) |
| `background(work, then)` | `work` on the worker pool, `then(result)` on the main loop: heavy work without stalling the loop |
| `now()` | The application's clock (a test's manual clock under `AppRunner({}, {.manual_time = true})`) |
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
--help-config           Every configuration key with type, default, range and description
--check-config          Validate the configuration file and exit (0 = valid)
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
telnet = ""              # remote console, e.g. "0.0.0.0:2323"
telnet_max_sessions = 4
telnet_idle_s = 0

[forge]                  # forge.v1 endpoints (empty = off; "tcp://127.0.0.1:*" = any free port)
control = ""             # requests from REQ clients (forgectl, panels), e.g. "tcp://*:5557"
events = ""              # PUB: state, alarms, log, audit, health, metrics
data = ""                # PUB: the application's streams (ephemeral port when empty and used)
metrics_interval_s = 5
control_workers = 2      # [forge].control requests handled at the same time
request_timeout_ms = 2000 # requests waiting for a busy main loop answer 503 after this

[auth]                   # users and roles for web, forge.v1 and telnet (see below)
enabled = false
default_role = "viewer"
# [auth.users.<name>] password / token / role

[audit]
history = 500
file = ""                # also append JSON lines

[trace]
enabled = true
capacity = 65536         # events in the flight recorder

[crash]
enabled = true
dir = ""                 # default: $STATE_DIRECTORY or /tmp

[state]
file = ""                # controls marked persist() survive restarts

[health]
watchdog_grace_s = 0     # stop systemd watchdog pings when a critical component failed this long

[process]
mlockall = false         # lock memory (real-time)

[hub]                    # forge-hub (optional): dashboard entry, see below
register = true
endpoint = "tcp://127.0.0.1:5550"
retry_s = 30             # 0 = try once
critical = false         # a failed unit is a critical alarm on the hub
group = ""
title = ""

[threads.<name>]         # app.configure_thread("<name>"): policy other|fifo|rr|batch|idle,
                         # priority, nice, cpus = [1, 2]
```

Unknown keys are reported with a suggestion (`htpp.port` → did you mean
`http.port`?); wrong types and out-of-range values stop the start-up (and a
reload) with a clear message. Applications declare their own keys in
`setup_config(forge::config::Schema&)`.

A default is written once, in the declaration: after loading, every declared
key the file leaves out is filled in with its default (`--print-config` shows
them), so declared keys are read with `cfg.value<T>("key")`, never with
`get_or("key", <the default again>)` (a test of forgecpp checks its sources for
that). `get_or` is for keys nobody declared. A default that depends on the
environment is marked `.computed()` and not filled in.

### `forge::Config` (toml++ + nlohmann/json)

```cpp
auto cfg = forge::Config::load_file("/etc/app/app.toml");
int port   = cfg.value<int>("http.port");                  // declared key: the schema's default
int extra  = cfg.get_or<int>("my.extra", 3);                // undeclared key: default when missing
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
    if (!body.contains("name")) throw forge::Error(422, "name is required");
    return forge::json{{"created", true}};
});

forge::http::Client client("http://10.0.0.5:8080");
client.timeout(2s).bearer_token(token);
forge::json items = client.get_json("/api/items");   // >= 400 -> HttpError
```

An invalid JSON body is answered with 400, an uncaught exception with 500 and
an unknown path with 404, each with a JSON error object.

Errors of requests are `forge::Error{status, message, details}`
(`<forge/error.hpp>`), whatever channel the request came in on: the control
registry, `ControlService`, forge-hub and handlers throw it, and each channel
maps it (HTTP status and body, `forge.v1.Reply.status`/`error`, console text).
`http::HttpError` is a `forge::Error`; `http::Client` throws it, and code that
throws it keeps working.

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
<div data-forge-layout="main">                                          <!-- movable, resizable cards -->
  <section class="forge-card"><h2>Receiver</h2>...</section>
</div>
<button data-forge-layout-reset>Reset layout</button>
```

**Movable cards.** The cards of the control panel (`/forge/`), of forge-hub's
page and of any container marked `data-forge-layout="<page>"` sit on a
12-column grid ([GridStack.js](https://gridstackjs.com), bundled): drag a card
by its title, resize it from its corner. A card keeps the height of its
content until it is resized by hand (then its content scrolls). The layout is
stored by the application on the device (`[web].layout_file`, default
`$STATE_DIRECTORY/web-layout.json` or `~/.local/state/<name>/web-layout.json`),
so every browser shows the same arrangement; without that it stays in the
browser. With `[auth]`, a signed-in user's changes are their own layout
(Reset goes back to the device's), and operators and admins change the
device's layout, the default for everyone, with `?scope=device`
(`PUT /forge/api/layout/<page>?scope=device`). On phones the cards are a plain list and the stored layout is left
alone. From JavaScript: `const l = await forge.layout(element, 'page')`,
`l.add(card)`, `l.remove(card)`, `l.reset()`.

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
| `GET/PUT/DELETE /forge/api/layout/<page>` | Card layout of a page (`{"items": [{id, x, y, w, h, fit}]}`) |
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

### forge.v1: one message standard for every application

The protocol is defined in [`proto/forge/v1`](proto/forge/v1) (installed in
`share/forgecpp/proto`). Python: the `forge-v1` package (`forge.v1.*_pb2`
and `forge.v1.framing`) is installed into site-packages; applications install
their own messages next to it with `forge_install_python(DIRECTORY <dir>)`
(the `PYTHON` dir of `forge_add_proto`), so scripts simply
`from forge.v1 import framing` and `from radio.v1 import radio_pb2` (from a
build directory: `PYTHONPATH=<build>/python`):

| Endpoint (`[forge]`) | Socket | Carries |
|---|---|---|
| `control` | ROUTER (REQ clients) | `forge.v1.Request` → `forge.v1.Reply`: info, schema, state, get/set/press, call (commands), alarms/ack, logs, metrics, trace, audit, health, streams, lease/release, log level |
| `events` | PUB | topics `state` (`Values`), `alarms`, `log`, `audit`, `health`, `metrics` |
| `data` | PUB | the application's own streams (its messages, e.g. `radio.v1.IqBlock`) |
| HTTP `POST /forge/api/rpc` | | the same `Request`/`Reply`, as protobuf (`application/x-protobuf`) or JSON |

Published messages are multipart: **topic**, **`forge.v1.Header`** (source,
host, pid, sequence number per topic, time, body type, trace context),
**body** (the protobuf message) and an optional **blob** (raw bytes such as IQ
samples, described by the body). Receivers count gaps in the sequence numbers;
the trace context continues across daemons, so a request can be followed
from the panel through every application it touches (audit records carry it too).

**forgectl** talks to any forgecpp application:

```sh
forgectl -e tcp://board:5557 info                 # name, version, endpoints, uptime
forgectl -e tcp://board:5557 controls             # every control and command with its value
forgectl -e tcp://board:5557 set rx_lo_mhz 2412
forgectl -e tcp://board:5557 call scan from=2400 to=2480
forgectl -e tcp://board:5557 alarms / ack all / logs -n 50 / audit / health / streams / metrics
forgectl -e tcp://board:5557 trace -o trace.json  # flight recorder for ui.perfetto.dev
forgectl -e tcp://board:5557 watch                # live events (state, alarms, logs, audit)
forgectl -e http://board:8080 state               # the same over HTTP
FORGE_ENDPOINT=tcp://board:5557 FORGE_TOKEN=... forgectl --json state
```

**Streams.** An application publishes its data with `stream()`; the stream is
listed by `forgectl streams` and on the panel (subscribers, sent, dropped):

```cpp
// proto/pump/v1/pump.proto: message Sample { double pressure = 1; ... }
forge_add_proto(pump ROOT proto PROTOS pump/v1/pump.proto PYTHON ${CMAKE_BINARY_DIR}/python)   // CMake

auto& samples = stream("samples", "pump.v1.Sample");            // on [forge].data
auto& iq = stream("iq", "radio.v1.IqBlock", {"latest", 20});    // newest only, at most 20/s
if (samples.wanted()) samples.publish(msg);                      // any thread; wanted(): someone listens
iq.publish(block, buffer, buffer->data(), bytes);                // zero copy while `buffer` (shared_ptr) lives
```

Receiving, in C++ or Python:

```cpp
forge::zmq::Receiver sub(forge::zmq::Receiver::Kind::sub, "tcp://board:5555",
    [this](forge::zmq::Message&& m) {
        auto s = m.as<pump::v1::Sample>();        // checks Header.type
        post([this, s] { use(s); });             // to the main loop
    }, {.topics = {"samples"}});
sub.lost();                                       // messages missed (sequence gaps)

forge::zmq::Client c("tcp://board:5557");         // forge.v1 control client (timeout + reconnect)
c.set("speed", 1500); c.press("flush"); c.call_command("scan", {{"to", 600}});
```

```python
topic, header, body, *blob = sock.recv_multipart()
h = common_pb2.Header(); h.ParseFromString(header)    # from forge.v1 import common_pb2
s = pump_pb2.Sample(); s.ParseFromString(body)
```

Lower-level pieces: `Publisher`/`Stream` (thread-safe PUB with per-topic
subscriber counts), `Sender` (PUB/PUSH from one thread), `Receiver` (SUB/PULL
thread), `Server` (ROUTER + worker threads), `serve_control(service, endpoint, workers)` for extra control
endpoints. The control endpoint still accepts the older JSON commands
(`{"cmd":"set","id":"speed","value":1500}`), deprecated: each one is counted
in `forge.control.legacy_json`, every client that sends them is named in a
warning once, and `[forge].legacy_json = false` refuses them (status 410).
They will be off by default in 0.4 and removed in 0.5; move scripts to
forge.v1 (`forgectl`, `forge::zmq::Client`, Python `forge.v1`).

### Access control, leases and the audit trail

Every way in (web, forge.v1 over ZeroMQ or HTTP, console, telnet) goes through
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
token = "..."                    # forgectl --token, Authorization: Bearer
role = "operator"
```

- **Roles:** `viewer` (read), `operator` (set, press, call, ack, lease),
  `admin` (log levels, controls/commands marked `.role(forge::Role::admin)`).
  Without `[auth]` everyone is admin (lab default); the legacy `[http.auth]`
  user becomes an admin.
- **Leases:** `forgectl lease rx_lo_mhz 60` (or `group:Radio`, or the panel's
  Leases page) keeps others from changing a control while you work with it;
  others get `409 leased by ...`.
- **Audit:** who (channel, client, user) changed what, old → new value,
  result and trace id: `forgectl audit`, the panel's Audit page, the
  `audit` event topic and optionally a JSON-lines file.
- **Persistence:** `.persist()` on a control keeps its value in `[state].file`.
- **Commands (RPC):** typed parameters with defaults and ranges:

```cpp
g.command("scan", "Scan", {forge::web::number_param("from", 2400).range(70, 6000),
                           forge::web::number_param("to")},
          [this](const forge::json& a) { return scan(a["from"], a["to"]); }).confirm("Start a scan?");
```

### Observability

| | |
|---|---|
| **Metrics** | `metrics().counter / gauge / histogram / gauge_fn`; the framework adds HTTP, main loop lag/busy, thread pool, alarms, health and process metrics. Prometheus text at `GET /metrics`, `forgectl metrics`, the panel's Metrics page (with trends), the `metrics` event topic |
| **Trace** | `FORGE_TRACE_SCOPE("name")`, `forge::trace::instant/counter`: a lock-free flight recorder per thread; main loop tasks and control requests are traced automatically. Download Chrome JSON (`forgectl trace`, panel Trace page, `GET /forge/api/trace?seconds=5`) and open it in ui.perfetto.dev |
| **Crash reports** | Fatal signals write a report (signal, backtrace, last log lines) to `[crash].dir`; the next start raises the `forge.crashed` alarm with its path |
| **Logs** | `[log] format = "json"` for collectors; per-logger levels (`forgectl log-level radio debug`); history via `forgectl logs`, live via `watch` and telnet `logs on` |
| **Health** | `health().set("radio", forge::HealthState::failed, "device lost")`; `/health` (`?strict=1` → 503), `forgectl health`, the panel's Health page; with `health.watchdog_grace_s` a failed critical component stops the systemd watchdog pings so systemd restarts the daemon |

### Remote console (telnet)

`[console] telnet = "0.0.0.0:2323"` serves the same console commands to
`telnet board 2323` (several sessions, each with its own output). With
`[auth]` the session asks for a user and password (not echoed) and commands
run with that user's role. `logs on|off` follows the log, `whoami`, `exit`.
Telnet is not encrypted: keep it on a trusted network, or bind it to
`127.0.0.1` and reach it through `ssh -L`.

### Components, configuration schema and real-time threads

```cpp
class RadioLink : public forge::Component {           // lifecycle + health in one place
public:
    RadioLink() : Component("radio", /*critical=*/true) {}
    void setup_config(forge::config::Schema& s) override { s.string("radio.uri", "", "libiio URI"); }
    void on_start() override { open(); set_health(forge::HealthState::ok); }
    void on_stop() override { close(); }
};
add_component<RadioLink>();                           // in the constructor

void setup_config(forge::config::Schema& s) override {
    s.integer("pump.max_rpm", 3000, "Speed limit").range(100, 6000).unit("rpm");
    s.string("pump.mode", "auto", "Control mode").choices({"auto", "manual"});
}

std::thread rx([this] { configure_thread("rx"); ... });   // [threads.rx] policy/priority/cpus
auto pool = forge::BufferPool<std::int16_t>::create(65536, 8);   // recycled buffers, no allocation per block
```

### Alarms (`forge::Alarms`)

A persistent, HMI/SCADA-style alarm list for conditions that need operator
attention. Unlike short-lived notifications (`notify_web`), an alarm stays
visible until an operator acknowledges it:

```cpp
// callable from any thread; raising the same id again does not create a new entry:
// message/time are updated and a counter increases (x12) - no flood of messages
alarms().raise("tank.level", forge::Severity::warning, std::format("Tank {:.0f} % full", level));
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
- Alarms are also published on the `alarms` event topic and listed by `forgectl alarms`.

### `forge::Console` (linenoise)

Enabled with `-i`; it is the backup control channel when the web UI is not
reachable. History, tab completion (commands and their arguments) and hints
(the rest of a command name, then its arguments from the help text) are
included; while the console is open, log lines and output from other threads
are printed above the line being typed without corrupting it. Piped input is
read line by line without a prompt (scripts: `printf 'status\nexit\n' | app -i`).
A terminal without a window size (serial consoles) is taken as `$COLUMNS` or
80 columns wide. Built-in commands: `help`, `status`, `log-level`, `config`, `reload`,
`controls`, `set`, `press`, `call`, `alarms`, `ack`, `lease`, `release`,
`metrics`, `health`, `streams`, `audit`, `trace`, `history`, `clear`, `exit`.
The same commands are available remotely over telnet (see above).

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
#include <forge/testing.hpp>     // doctest + TempDir + wait_until + AppRunner

TEST_CASE("config defaults") {
    auto cfg = forge::Config::parse("port = 80");
    CHECK(cfg.get_or<int>("port", 0) == 80);
}
```

```cmake
forge_add_test(mydaemon_tests SOURCES tests/test_main.cpp)   # main() is provided
```

Run a whole application in a test, on free ports, and talk to it like a client would:

```cpp
forge::testing::AppRunner<Pump> r({"-D", "forge.control=tcp://127.0.0.1:*"});
forge::zmq::Client c(r->endpoints().at("control"));
CHECK(c.set("speed", 1500) == 1500);
```

Timer-driven logic without sleeping (alarms with hysteresis, rate limits,
timeouts): the test owns the clock. Every timer due on the way runs on the
main loop, in order, and reads `now()` == the time it was due at:

```cpp
forge::testing::AppRunner<Pump> r({}, {.manual_time = true});
r->invoke([&] { r->pressure = 9.5; });
r.advance(2s);                         // not long enough for the alarm
CHECK_FALSE(r->alarms().active("overpressure"));
r.advance(1s);
CHECK(r->alarms().active("overpressure"));
r.advance(24h);                        // a day of timers in a few seconds
```

For that, time-based code reads `now()` (the application's clock), not
`std::chrono::steady_clock::now()`. `wait_until()` timeouts scale with
`$FORGE_TEST_TIMEOUT_SCALE` for slow builds (sanitizers, emulation).
Sanitizers: `-DFORGECPP_SANITIZE=address;undefined` (or `thread`), or
`install.sh --sanitize thread` to instrument the dependencies as well.

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
  `forge.v1.AppList` on the hub's `apps` stream.
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

## Yocto / embedded deployment

forgecpp is meant to run on embedded Linux. The whole stack cross-compiles with
a Yocto-style toolchain; on aarch64 the full test suite passes and the example
daemons run with their web UI. Stripped sizes on aarch64: `libforgecpp.so`
≈ 2.1 MB, `libspdlog.so` ≈ 650 KB (line editing is compiled into libforgecpp); a scaffolded
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
| `examples/sensor-service` (`sensor-service`) | Every building block: channel, thread pool, events, control panel, own page, alarm with hysteresis |
| `examples/pipeline` | **Multi-threaded application:** N producer threads → lock-free `Channel` → dispatcher thread → `ThreadPool` workers → result channel → main loop. Bounded in-flight work (backpressure), atomic counters, alarms raised from worker threads, clean shutdown |
| [`examples/radio-controller`](examples/radio-controller/README.md) | **AD936x radio daemon (libiio + forge.v1):** controls the transceiver (LO, sample rate, bandwidth, AGC/gain, TX attenuation) through libiio and streams IQ samples as `radio.v1.IqBlock` streams (zero copy from pooled DMA buffers; TX via PULL with `radio.v1.TxBlock`); the channels it listens to, drives and shares come from its configuration (e.g. RX2 for the spectrum analyzer). Real-time RX thread settings, metrics, health. Live spectrum and waterfall page, alarms for overflows, underflows, temperature and a lost device (with automatic reconnect), state persistence, a hardware simulator, Python clients, systemd unit and its own Yocto recipe |
| [`examples/spectrum-analyzer`](examples/spectrum-analyzer/README.md) | **AD936x spectrum analyzer with a web UI** (on its own RX path, or on RX2 next to the radio controller): realtime (one capture) or swept (LO stepping) spans from 10 kHz to 70 MHz - 6 GHz, RBW/window/video averaging, peak/sample/average/negative-peak detectors, three traces (clear/write, max hold, min hold, average, view), four markers with delta, peak search/next/left/right, peak track and signal track, waterfall, channel power and occupied bandwidth, limit line alarm, CSV/PNG/IQ export, `sa.v1.Spectrum` stream. Shares the AD936x backend with the radio controller |
| [`examples/qam-modem`](examples/qam-modem/README.md) | **16QAM packet modem over an AD936x:** UDP packets from Ethernet go on the air through radio-controller and come back out over UDP: 750 ksym/s, about 1 MHz wide at 2.1 GHz, 2.9 Mbit/s net. Schmidl & Cox detection and frequency offset, fractional timing, phase loop, CRC-checked packets; TX paced by the RX sample clock; constellation page, test mode, `echo.py` traffic checker. DSP tested through an impaired channel; end-to-end test at 2.3 Mbit/s with the simulator |
| [`examples/channel-emulator`](examples/channel-emulator/README.md) | **Radio channel emulator in the TX path:** between a sender (qam-modem) and radio-controller; noise at an SNR, frequency offset and Doppler, phase noise, sample clock error, multipath, Rayleigh/Rician fading, IQ imbalance, DC, pulsed interference, each a control, plus presets. Every receiver behind the splitter sees the result (the modem's EVM, the analyzer's spectrum). Each impairment measured in tests; the modem's packet error rate per preset |
| `forgecpp-new` | Scaffolds an application with all of the above: its own proto + stream, commands, schema, `[forge]` endpoints, telnet, an `AppRunner` test and a Python client |
| `examples/thermostat` | **Web-first application:** its own multi-page site (overview, live SVG chart, settings form), custom REST API with per-field validation, live data via `publish_web`, radio/segmented controls, alarms, settings persisted with `save_json` |

```sh
./install.sh --prefix ~/.local --examples
cd build/forgecpp/examples
./pipeline -c pipeline.toml --producers 4 -i      # http://localhost:8080/forge/
./thermostat -c thermostat.toml                    # http://localhost:8080/
./sensor-service -c sensor-service.toml -i --simulate-spikes
./radio-controller/radio-controller -c radio-controller/radio-controller.toml   # simulated AD9364
./radio-controller/radio-controller --backend iio --uri ip:192.168.2.1         # real AD936x via iiod
./spectrum-analyzer/spectrum-analyzer -c spectrum-analyzer/spectrum-analyzer.toml  # http://localhost:8081/
./spectrum-analyzer/spectrum-analyzer -c spectrum-analyzer/spectrum-analyzer.toml --backend radio  # RX2 of the radio controller
./qam-modem/qam-modem -c qam-modem/qam-modem.toml   # 16QAM modem through the radio controller: http://localhost:8082/
./channel-emulator/channel-emulator -c channel-emulator/channel-emulator.toml   # impairments for the modem's TX: http://localhost:8083/forge/
```

The radio controller and the spectrum analyzer are also standalone CMake
projects (`cmake -S examples/radio-controller -B build
-DCMAKE_PREFIX_PATH=<forgecpp prefix>`) that install the daemon, its config
and systemd unit; their Yocto recipes in `yocto/recipes-connectivity` build
them that way. Both use `examples/common/ad936x` (libiio backend, simulator,
FFT/windows).

Every example has a generated systemd unit next to its binary
(`build/forgecpp/examples/<name>.service`, `.../examples/<name>/<name>.service`
for radio-controller, spectrum-analyzer, qam-modem and channel-emulator) with a restart policy that suits it:
hello on-failure without a watchdog, sensor-service and pipeline on-failure
with a watchdog, thermostat `always`, radio-controller, spectrum-analyzer,
qam-modem and channel-emulator on-failure (3 in 10 minutes) with a 30 s watchdog.

## Build times

A source file including `<forge/forge.hpp>` takes about 8 s to compile (-O2;
cpp-httplib is a compiled library, spdlog and protobuf too). For quicker
edit-compile cycles while developing, `-DFORGE_PCH=ON` precompiles the header
for every `forge_add_app` / `forge_add_test` target (one file edited in the
pump example: 17.8 -> 11.9 s); it makes clean builds of small projects slower,
so it is off by default. Include only the headers you need
(`<forge/application.hpp>`, `<forge/web.hpp>`, ...) in files that do not need
everything.

## Versions

Semantic versions, changes in [CHANGELOG.md](CHANGELOG.md). Until 1.0 a minor
version may change the C++ API: `libforgecpp.so.<major>.<minor>` and
`find_package(forgecpp 0.3)` accepts 0.3.x only. The forge.v1 protocol only
changes compatibly (new fields and messages; removed numbers reserved): CI
checks it against the last release's API (`proto/forge/v1/API.json`) with
`tools/proto-compat.py`, which also records it at a release (`--update`). Within a minor
series the C++ API and the configuration keys stay compatible too
(`tools/api-check.py`, `api/forgecpp-<x.y>.json`); what counts as public and
how it may change: [docs/API.md](docs/API.md).

## Updating dependencies

```sh
git -C third_party/spdlog fetch --tags && git -C third_party/spdlog checkout v1.x.y
git add third_party/spdlog && git commit -m "spdlog: v1.x.y"
```

## Directory layout

```
include/forge/     public headers (forge.hpp includes everything)
proto/forge/v1/    forge.v1 message standard (common, observability, control)
src/               libforgecpp sources
src/web/           built-in control panel, forge.js, forge.css (embedded in the library)
third_party/       submodules
cmake/             package config, pkg-config, forge_add_app/forge_add_web/forge_add_test
tools/             forgecpp-new (project scaffolder), forgectl (forge.v1 client)
hub/               forge-hub: device dashboard and registry (a product, not an example)
examples/          hello, sensor-service, pipeline (multi-threaded), thermostat (web site),
                   radio-controller (AD9364 over libiio + ZeroMQ),
                   spectrum-analyzer (AD936x spectrum analyzer), qam-modem (16QAM over
                   radio-controller), channel-emulator, common/ad936x, common/channel
tests/             doctest unit tests
yocto/             bitbake recipes (forgecpp, radio-controller, spectrum-analyzer, qam-modem,
                   channel-emulator)
install.sh, get.sh installation
```

## License

MIT (see [LICENSE](LICENSE)). The bundled libraries come with their own
licenses (MIT, BSD-2/3-Clause, BSL-1.0; protobuf BSD-3-Clause; GridStack.js MIT in
`src/web/vendor/gridstack`; libzmq MPL-2.0; libiio LGPL-2.1-or-later
with GPL-2.0-or-later command line tools); see `third_party/*/LICENSE*` and
`third_party/libiio/COPYING*.txt`. Link libzmq and libiio dynamically (the
default) to keep the LGPL/MPL obligations simple.
