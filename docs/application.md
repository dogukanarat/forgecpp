# The application and its services

What a `forge::Application` gives, and the libraries behind it. The web panel is in [web.md](web.md), commands and streams in [messaging.md](messaging.md), access control, observability and forge-hub in [operations.md](operations.md).

New to forgecpp? The [tutorials](tutorials/README.md) build a pump
controller step by step (its code is `examples/pump`, compiled and tested).
API reference: `docs/build-api.sh` (or `cmake --build build --target
forgecpp_docs`), then `build/docs/html/index.html`.

Threads in one sentence: the application's state belongs to its main loop;
other threads reach it with `invoke()` / `post()`, heavy work leaves it with
`background()`. Where each callback runs and which call to use:
[docs/threading.md](threading.md).

## `forge::Application`

`run(argc, argv)` does the following, in order:

1. Parses the standard flags and your own options with CLI11
2. Loads the TOML configuration and applies `-D key=value` overrides and flags
3. Sets up logging from the `[log]` section (console / rotating file / syslog)
4. Starts a thread pool with `[threads].workers` threads
5. Prepares the HTTP server (`[http]` or `--http`), the web UI (`[web]`) and the console (`-i`)
6. Calls `on_start()` and enters the **main loop**
7. `SIGINT`/`SIGTERM` → graceful shutdown, `SIGHUP` → `on_reload()`, a second signal → forced exit
8. Reports `READY=1`/`STOPPING=1` to systemd and feeds the watchdog when `WatchdogSec=` is set

All timers, tasks submitted with `post()`/`invoke()`, events published with
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
| `invoke(fn) -> R` | Run on the main loop and wait for the result (runs inline if already on the main loop; use this in HTTP handlers) |
| `invoke_for(timeout, fn) -> R` | Same, but gives up with `forge::Error(503, "main loop busy")` when the main loop has not started `fn` in time (`fn` then never runs). The framework's request handlers use it with `request_timeout()` (`[loop].request_timeout_ms`) |
| `background(work, then)` | `work` on the worker pool, `then(result)` on the main loop: heavy work without stalling the loop |
| `now()` | The application's clock (a test's manual clock under `AppRunner({}, {.manual_time = true})`) |
| `after(d, fn)`, `every(d, fn)`, `cancel(id)` | Timers (run on the main loop) |
| `emit(Event{...})`, `events().subscribe<Event>(...)` | Typed event bus |
| `config()` | Snapshot of the effective configuration |
| `alarms().raise(id, severity, message)`, `alarms().clear(id)` | Alarm management (from any thread) |
| `publish_web(event, json)`, `notify_web(message, level)` | Send an event / a short notification to open web pages |
| `logger()`, `pool()`, `http()`, `dashboard()`, `console()`, `cli()` | Services |
| `worker(name, body)` | A named thread of the application (`[threads.<name>]` applied, stopped before `on_stop()`, health and an alarm if its body throws) |
| `stop(code)`, `reload()` | Thread-safe lifecycle control |

Overridable hooks: `setup_config`, `setup_cli`, `setup_controls`, `setup_http`, `setup_console`,
`on_start`, `on_stop`, `on_reload`, in this order at start-up: `setup_cli`
(the command line is parsed) → `setup_config` → the file is loaded and
validated → `setup_controls` (the control registry: panel, forgectl, console, telnet,
links) → the persisted values (`[state].file`) → `setup_http` / `setup_console`
→ `on_start` → the links and the HTTP server start taking requests. A
control's setter must therefore work before `on_start()`. If you prefer not to
subclass, the same hooks are available as lambdas: `hook_config`, `hook_cli`,
`hook_controls`, `hook_start`, `hook_stop`, `hook_reload`,
`hook_http`, `hook_web`, `hook_console` (see `examples/hello`).

### Standard command line

```
-c,--config FILE        Configuration file (otherwise ./<name>.toml, <install>/etc/<name>/<name>.toml, /etc/<name>/<name>.toml are searched)
-D,--set KEY=VALUE      Override a config value (TOML value: 42, true, "x", [1,2]; anything else is a string)
-l,--log-level LEVEL    trace|debug|info|warn|error|critical|off
-v / -vv                debug / trace
--log-file FILE         Rotating log file
--syslog                Also log to syslog
-i,--interactive        Interactive console
--instance NAME         Instance name (default: the instance of a systemd template unit, or $FORGE_INSTANCE)
--http [HOST:]PORT      Enable the HTTP server (includes GET /health)
--print-config          Print the effective configuration as JSON and exit
--help-config           Every configuration key with type, default, range and description
--check-config          Validate the configuration file and exit (0 = valid)
-V,--version
```

### Configuration keys read by the framework

```toml
[log]
level = "info"
file = ""                # empty = disabled
max_size_mb = 10
max_files = 3
syslog = false
console = true
pattern = "[{time}] [{level}] [{logger}] {message}"   # also {thread} {file} {line}; {level}: TRC DBG INF WRN ERR CRT
color = "auto"           # level colours: auto (a terminal, no NO_COLOR) | always | never
format = "text"          # text | json (one object per line)
history = 500            # records kept for the panel, forgectl logs and telnet
flush_level = "warn"     # records at or above are written out at once
flush_interval_s = 2     # the others, at most this late

[threads]
workers = 0              # 0 = number of CPUs

[http]
enabled = false
host = "0.0.0.0"
port = 8080
threads = 12             # each open browser tab holds one thread for live updates (max_clients + 4)

[http.auth]              # older single-user credentials; prefer [auth] (users and roles, below)
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
layout_file = ""         # card layouts of the pages (default: $STATE_DIRECTORY/web-layout.json; "-" = memory only)
live_reload = true       # pages served from `root` reload when a file changes

[console]
enabled = false          # same as -i
prompt = "<name>> "
history = "~/.<name>_history"
color = true             # default: on a terminal
telnet = ""              # remote console, e.g. "0.0.0.0:2323"
telnet_max_sessions = 4
telnet_idle_s = 0

[forge]                  # the links (ZeroMQ; empty = off; "tcp://127.0.0.1:*" = any free port)
control = ""             # commands (c2 frames) from REQ clients (forgectl, programs), e.g. "tcp://*:5557"
events = ""              # PUB: state, alarms, log, audit, health, metrics
data = ""                # PUB: the application's streams (ephemeral port when empty and used)
metrics_interval_s = 5
control_workers = 2      # [forge].control requests handled at the same time

[serial]                 # the same commands on a serial line (zephyrus framing)
device = ""              # e.g. "/dev/ttyUSB0"; opened again when it goes away
baud = 115200
frame_size = 244         # the peer's CONFIG_ZEPHYRUS_COBS_FRAME_SIZE

[can]                    # the same commands on a CAN bus, over ISO-TP (docs/hardware-io.md)
interface = ""           # e.g. "can0"; opened again when it goes away
tx_id = 1793             # 0x701: the id this program answers with
rx_id = 1792             # 0x700: the id it listens to (the peer's tx id)
extended = false         # 29-bit ids
fd = false               # CAN FD frames under ISO-TP
frame_size = 244         # largest framed piece in one ISO-TP message

[loop]                   # the main loop
slow_task_ms = 200       # a task longer than this is logged with its duration (0 = never)
request_timeout_ms = 2000 # requests waiting for a busy main loop answer 503 after this
max_pending_tasks = 10000 # beyond this post() drops (forge.loop.dropped)

[health]
stall_ms = 5000          # /health says "stalled" when the loop's heartbeat is older
watchdog_grace_s = 0     # a failed critical component stops the watchdog pings after this (0 = never)

[auth]                   # users and roles for web, the command link and telnet (see below)
enabled = false
anonymous = true         # false: HTTP needs credentials for everything but /health and /metrics
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

The old names `[forge].serial*`, `[forge].slow_task_ms`,
`[forge].request_timeout_ms` and `[forge].max_pending_tasks` still work, with
a warning (`Schema::alias`). A test (`tools/check-config-docs.py`) keeps this
table complete: every key `--help-config` prints is named here.

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

### Reloading

`SIGHUP`, the console's `reload` and `Application::reload()` read the file
again, validate it (a wrong value keeps the current configuration and logs
why) and apply:

| Applied again | Not applied until a restart |
|---|---|
| `[log].level`; the log file is opened again (logrotate: `postrotate kill -HUP`) | the other `[log]` keys, `[log].file` itself |
| `[loop].request_timeout_ms`, `[loop].slow_task_ms`, `[loop].max_pending_tasks` | the endpoints (`[forge].control`, `events`, `data`), `[serial]`, `[can]`, `[forge].control_workers` |
| `[health].watchdog_grace_s` | `[http]`, `[web]`, `[console]`, `[auth]`, `[threads]`, `[crash]`, `[state]`, `[hub]`, `[audit]`, `[trace]` |
| the application's own keys: `on_reload(config)` and the components' | |

`--help-config` lists every key; a key that is only read at start-up says so
in its description where it matters.

## `forge::Config` (toml++ + nlohmann/json)

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

## `forge::log` (Quill)

```cpp
forge::log::info("connected to {}:{}", host, port);   // std::format, checked at compile time
auto net = forge::log::get("net");       // named logger sharing the same outputs
net->debug("packet {}", id);
FORGE_WARN("disk {:.0f} % full", used);   // the caller's file and line ({file}:{line} in the pattern)
forge::log::set_level("debug");           // at runtime
```

The message is formatted on the calling thread and handed to a queue; Quill's
background thread writes it to the terminal (in colour), the file, syslog, the
history (web panel, RPC, telnet) and the listeners, so a log call never waits
for a terminal or a disk. Real-time threads use `FORGE_RT_*` from
`<forge/log_rt.hpp>`: the arguments are copied and formatted on the background
thread, the queue drops instead of blocking when it is full. Under systemd
(journald) each line carries its syslog priority instead of colours.

## `forge::Channel<T>` (moodycamel::ConcurrentQueue)

```cpp
forge::Channel<Frame> frames;
frames.push(Frame{...});                   // producer thread
while (auto f = frames.pop()) handle(*f);  // consumer; ends after close() once drained
auto f = frames.try_pop();                 // non-blocking
auto g = frames.pop_for(100ms);            // with timeout
```

## `forge::ThreadPool` (BS::thread_pool)

```cpp
auto fut = pool().submit_task([] { return compute(); });
pool().detach_loop(0, n, [&](std::size_t i) { process(i); });
pool().wait();
```

## `forge::http` (cpp-httplib + nlohmann/json)

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
maps it (HTTP status and body, a NACK with the errno of the status on the
link, console text).
`http::HttpError` is a `forge::Error`; `http::Client` throws it, and code that
throws it keeps working.

## Remote console (telnet)

`[console] telnet = "0.0.0.0:2323"` serves the same console commands to
`telnet board 2323` (several sessions, each with its own output). With
`[auth]` the session asks for a user and password (not echoed) and commands
run with that user's role. `logs on|off [level]` follows the log (levels in colour; `color off` for plain text), `whoami`, `exit`.
Telnet is not encrypted: keep it on a trusted network, or bind it to
`127.0.0.1` and reach it through `ssh -L`.

## Components, configuration schema and real-time threads

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

## Alarms (`forge::Alarms`)

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

## `forge::Console` (linenoise)

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

## Testing

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
forge::Remote pump(r->endpoints().at("control"));
CHECK(pump.set("speed", 1500) == 1500);
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
