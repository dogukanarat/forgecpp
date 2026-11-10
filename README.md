# forgecpp

A **batteries-included** C++20 framework for Linux daemons and services.

Best-in-class libraries are bundled as git submodules, installed with a single
`install.sh`, and wired together into an application skeleton under the
`forge::` namespace. Every daemon you build shares the same foundation:
logging, configuration, CLI, thread pool, HTTP, **web UI**, **ZeroMQ**,
console, signal handling and systemd integration.

Every application is operated, observed and connected **the same way**, so an
engineer moving between applications finds the same tools everywhere:

- **One message standard, binary and JSON.** Every command is a request and a
  reply with a number, defined in a `.proto` file with fixed bounds (nanopb: no
  allocation). Links (ZeroMQ, a serial line, a CAN bus over ISO-TP) carry them as c2 frames, byte
  for byte like [zephyrus](https://github.com/embedded-aviation/zephyrus) on
  the microcontrollers; the web sends the same commands as JSON. One handler
  answers both. Streams carry a header with source, sequence number, time and
  trace context, so data can be followed from one daemon to the next and gaps
  are detected.
- **The same ways in.** You declare controls and commands once in C++;
  forgecpp publishes them as a web control panel (`/forge/`), `data-forge-*`
  attributes for your own pages, the command link (`forgectl`), the local
  console (`-i`) and a **remote console over telnet**. All of them go through one
  control service with roles, leases and an audit trail.
- **Built-in observability.** Metrics (Prometheus `/metrics`), a flight
  recorder of trace spans (Perfetto), crash reports, structured (JSON) logs,
  health checks tied to the systemd watchdog.
- **The hardware every subsystem has.** GPIO lines (the kernel's character
  device, with a simulated chip for PCs and tests) and CAN (raw frames, and
  commands over ISO-TP), without extra libraries; optional components for
  SQLite (with a store-and-forward backlog) and LZ4.

```cpp
#include <forge/forge.hpp>
using namespace std::chrono_literals;

class Pump : public forge::Application {
public:
    Pump() : Application({"pump", "1.0.0", "Pump control service"}) {}

protected:
    void setup_controls(forge::Controls& ui) override {
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
| [Quill](https://github.com/odygrd/quill) | v13.1.0 | Asynchronous logging (header-only): a background thread writes the terminal, file, syslog and history | `forge::log`, `FORGE_INFO(...)`, `FORGE_RT_*` |
| [CLI11](https://github.com/CLIUtils/CLI11) | v2.7.2 | Command line parsing | `Application::cli()`, `setup_cli()` |
| [linenoise](https://github.com/antirez/linenoise) | a473823 (2026-05) | Console line editing: history, tab completion, hints, output while typing (compiled into libforgecpp) | `forge::Console` |
| [BS::thread_pool](https://github.com/bshoshany/thread-pool) | v5.1.0 | Lightweight, fast thread pool | `forge::ThreadPool`, `Application::pool()` |
| [moodycamel::ConcurrentQueue](https://github.com/cameron314/concurrentqueue) | v1.0.5 | Lock-free inter-thread data transfer | `forge::Channel<T>` |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.12.0 | JSON reading/writing | `forge::json`, `save_json()` |
| [toml++](https://github.com/marzer/tomlplusplus) | v3.4.0 | TOML configuration | `forge::Config` |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | v0.65.0 | HTTP/HTTPS client and server | `forge::http::Server`, `forge::http::Client` |
| [doctest](https://github.com/doctest/doctest) | v2.5.3 | Unit testing | `forge/testing.hpp`, `forge_add_test()` |
| [libzmq](https://github.com/zeromq/libzmq) + [cppzmq](https://github.com/zeromq/cppzmq) | v4.3.5 / v4.11.0 | ZeroMQ messaging (pub/sub, push/pull, request/reply) | `forge/zmq.hpp`, `[forge]` endpoints |
| [nanopb](https://github.com/nanopb/nanopb) | 0.4.9.2 | Protobuf messages as C structs with fixed bounds (compiled in) | `forge/pb.hpp`, `proto/forge`, `forge_add_proto()` |
| [protobuf](https://github.com/protocolbuffers/protobuf) | v21.12 | `protoc`, on the build machine only (nothing links libprotobuf) | `forge_add_proto()` |
| [libiio](https://github.com/analogdevicesinc/libiio) | v0.26 | Industrial I/O devices (ADI transceivers, ADCs, ...) | `examples/radio-controller`, `examples/spectrum-analyzer` |

## Installation

Requirements: CMake ≥ 3.16, a C++20 compiler (GCC ≥ 13 / Clang ≥ 17 with libstdc++ 13), git,
libxml2 (for libiio; skip with `--without-iio`). Python 3 with `protobuf`
(the message generator runs at build time; `apt install python3-protobuf`), and
`pyzmq` for the Python clients (optional).
Optional: `ninja`, OpenSSL (for HTTPS), zlib (for compression), libusb-1.0 (`--iio-usb`).

```sh
git clone --recursive https://github.com/dogukanarat/forgecpp.git
cd forgecpp
./install.sh                         # installs into /usr/local (uses sudo if needed)
```

One-liner (clones into a temporary directory, installs and cleans up):

```sh
curl -fsSL https://raw.githubusercontent.com/dogukanarat/forgecpp/master/get.sh | bash
curl -fsSL https://raw.githubusercontent.com/dogukanarat/forgecpp/master/get.sh | bash -s -- --prefix /opt/forgecpp --ref v0.4.0
```

Every `install.sh` option, cross builds and Yocto: [docs/installation.md](docs/installation.md).

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

`forgecpp-new --minimal my-daemon` makes the smallest shape: a schema, the
controls, a timer, the configuration and tests, about 100 lines of C++ (no
messages, web page, Python client, CLI flag, REST route, console command,
action, metric or trace span); `--with-command` and `--with-stream` add a
command or a stream back.

Generated project:

```
my-daemon/
├── CMakeLists.txt              find_package(forgecpp) + forge_add_app() (systemd unit with
│                               its restart policy) + forge_proto + forge_add_test()
├── src/my_daemon.hpp, main.cpp forge::Application subclass (schema, controls, commands, stream)
├── proto/my_daemon/my_daemon.proto   its own messages (a "status" command, a "ticks" stream)
├── web/index.html              the application's own page (embedded in the binary)
├── config/my-daemon.toml       default configuration
├── tests/test_my_daemon.cpp    doctest + forge::testing::AppRunner
├── clients/watch.py            Python client of the stream (forge package)
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

## Documentation

| | |
|---|---|
| [Tutorials](docs/tutorials/README.md) | A pump controller, step by step: controls, configuration, timers, alarms, threads, testing, streams, deployment |
| [The application and its services](docs/application.md) | `forge::Application`, the command line, configuration keys, logging, channels, the thread pool, HTTP, alarms, the console, testing |
| [The web panel](docs/web.md) | The control panel at `/forge/`, `data-forge-*` attributes for your own pages |
| [Commands and streams](docs/messaging.md) | The message standard; [adding a command](docs/commands.md); [the wire format](docs/protocol.md) |
| [Running it](docs/operations.md) | Access control, leases, the audit trail, observability, forge-hub |
| [Threads](docs/threading.md) | Which thread may do what, and how to hand work between them |
| [GPIO and CAN](docs/hardware-io.md) | Lines, edges, the simulated chip; CAN frames, commands over ISO-TP, vcan |
| [Storage and compression](docs/storage.md) | SQLite, store and forward (`forge::Backlog`), LZ4 |
| [Installation](docs/installation.md) | Every `install.sh` option, cross builds, Yocto, build times |
| [Working on forgecpp](docs/development.md) | `tools/check.sh`, presets, fuzzing, versions, the repository layout |
| [API rules](docs/API.md), [coding standards](docs/CODING_STANDARDS.md), [roadmap](docs/ROADMAP.md), [changes](CHANGELOG.md) | |

## Examples

| Example | What it shows |
|---|---|
| `examples/hello` | The smallest daemon (lambda style), a console command |
| `examples/sensor-service` (`sensor-service`) | Every building block: channel, thread pool, events, control panel, own page, alarm with hysteresis |
| `examples/pipeline` | **Multi-threaded application:** N producer threads → lock-free `Channel` → dispatcher thread → `ThreadPool` workers → result channel → main loop. Bounded in-flight work (backpressure), atomic counters, alarms raised from worker threads, clean shutdown |
| `examples/three-threads` | **Three threads in one file:** `acquire` (a dummy sensor) → `Channel` → `process` (blocks) → `Channel` → `downlink`, results into the main loop with `post()`. Named threads with `worker()` (`[threads.<name>]` priorities, clean stop, a thread that throws shows in health and alarms), atomics for what the panel changes, a test that drives it through its controls |
| `examples/payload-power` | **GPIO:** a payload's power switch (EN) and power-good (PGOOD) as a state machine: edges from a worker thread, retries, alarms, a brown-out; runs on the simulated chip on a PC, `[payload].chip = "gpiochip0"` on a board |
| `examples/can-node` | **CAN:** a node with a heartbeat frame, a sensor's temperature frames (alarm above a limit) and its commands over ISO-TP; `sensor.py` is the other node (vcan on a PC) |
| `examples/telemetry-store` | **SQLite + LZ4, store and forward:** housekeeping records wait in a backlog file between passes; during a pass they go out in LZ4 batches (`lz4 -dc` opens them); survives restarts, drops the oldest when full |
| [`examples/radio-controller`](examples/radio-controller/README.md) | **AD936x radio daemon (libiio + ZeroMQ):** controls the transceiver (LO, sample rate, bandwidth, AGC/gain, TX attenuation) through libiio and streams IQ samples as `radio.iq_block` streams (zero copy from pooled DMA buffers; TX via PULL with `radio.tx_block`); the channels it listens to, drives and shares come from its configuration (e.g. RX2 for the spectrum analyzer). Real-time RX thread settings, metrics, health. Live spectrum and waterfall page, alarms for overflows, underflows, temperature and a lost device (with automatic reconnect), state persistence, a hardware simulator, Python clients, systemd unit and its own Yocto recipe |
| [`examples/spectrum-analyzer`](examples/spectrum-analyzer/README.md) | **AD936x spectrum analyzer with a web UI** (on its own RX path, or on RX2 next to the radio controller): realtime (one capture) or swept (LO stepping) spans from 10 kHz to 70 MHz - 6 GHz, RBW/window/video averaging, peak/sample/average/negative-peak detectors, three traces (clear/write, max hold, min hold, average, view), four markers with delta, peak search/next/left/right, peak track and signal track, waterfall, channel power and occupied bandwidth, limit line alarm, CSV/PNG/IQ export, `sa.spectrum` stream. Shares the AD936x backend with the radio controller |
| [`examples/qam-modem`](examples/qam-modem/README.md) | **16QAM packet modem over an AD936x:** UDP packets from Ethernet go on the air through radio-controller and come back out over UDP: 750 ksym/s, about 1 MHz wide at 2.1 GHz, 2.9 Mbit/s net. Schmidl & Cox detection and frequency offset, fractional timing, phase loop, CRC-checked packets; TX paced by the RX sample clock; constellation page, test mode, `echo.py` traffic checker. DSP tested through an impaired channel; end-to-end test at 2.3 Mbit/s with the simulator |
| [`examples/channel-emulator`](examples/channel-emulator/README.md) | **Radio channel emulator in the TX path:** between a sender (qam-modem) and radio-controller; noise at an SNR, frequency offset and Doppler, phase noise, sample clock error, multipath, Rayleigh/Rician fading, IQ imbalance, DC, pulsed interference, each a control, plus presets. Every receiver behind the splitter sees the result (the modem's EVM, the analyzer's spectrum). Each impairment measured in tests; the modem's packet error rate per preset |
| `forgecpp-new` | Scaffolds an application with all of the above: its own proto + stream, commands, schema, `[forge]` endpoints, telnet, an `AppRunner` test and a Python client |
| `examples/thermostat` | **Web-first application:** its own multi-page site (overview, live SVG chart, settings form), custom REST API with per-field validation, live data via `publish_web`, radio/segmented controls, alarms, settings persisted with `save_json` |

```sh
./install.sh --prefix ~/.local --examples
cd build/forgecpp/examples
./pipeline -c pipeline.toml --producers 4 -i      # http://localhost:8080/forge/
./three-threads -c three-threads.toml -i          # http://localhost:8080/forge/
./payload-power -c payload-power.toml             # GPIO on the simulated chip
./can-node -c can-node.toml                       # on vcan0 (docs/hardware-io.md); python3 sensor.py vcan0
./telemetry-store -c telemetry-store.toml         # "Contact" on the panel starts a pass
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
hello on-failure without a watchdog, sensor-service, pipeline, three-threads,
payload-power, can-node and telemetry-store on-failure with a watchdog, thermostat `always`, radio-controller, spectrum-analyzer,
qam-modem and channel-emulator on-failure (3 in 10 minutes) with a 30 s watchdog.

## License

MIT (see [LICENSE](LICENSE)). The bundled libraries come with their own
licenses (MIT, BSD-2/3-Clause, BSL-1.0; nanopb Zlib; GridStack.js MIT in
`src/web/vendor/gridstack`; libzmq MPL-2.0; libiio LGPL-2.1-or-later
with GPL-2.0-or-later command line tools); see `third_party/*/LICENSE*` and
`third_party/libiio/COPYING*.txt`. Link libzmq and libiio dynamically (the
default) to keep the LGPL/MPL obligations simple.
