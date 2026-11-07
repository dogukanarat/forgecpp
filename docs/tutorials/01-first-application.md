# 1. Your first application

Install forgecpp once (`./install.sh --prefix ~/.local`), then create a project:

```sh
forgecpp-new pump -d ~/src        # ~/src/pump: sources, proto, page, config, tests, client
cd ~/src/pump
cmake -B build -DCMAKE_PREFIX_PATH=~/.local
cmake --build build -j
ctest --test-dir build
```

An application is a class derived from `forge::Application`. The smallest
one, and the start of [`examples/pump/pump.hpp`](../../examples/pump/pump.hpp):

```cpp
#include <forge/forge.hpp>

class Pump : public forge::Application {
public:
    Pump() : Application({"pump", "1.0.0", "Tutorial pump controller"}) {}
};

FORGE_MAIN(Pump)
```

The name (`pump`: lowercase words separated by `-`) names the binary, the
configuration file `pump.toml`, the systemd unit and the entry on the device's
dashboard. `FORGE_MAIN` defines `main()`; `run()` parses the command line, loads
the configuration, starts everything and runs until SIGTERM / Ctrl-C.

```cmake
find_package(forgecpp 0.3 REQUIRED)
forge_add_app(pump SOURCES src/main.cpp CONFIG config/pump.toml)
```

Run it:

```sh
./build/pump -D http.enabled=true -i        # -D sets any configuration key, -i: console
```

- `http://localhost:8080/forge/`: the control panel (state, alarms, logs, metrics, trace)
- `pump> help`: the console; `status`, `config`, `metrics`, `log-level debug`
- `./build/pump --help-config`: every configuration key with type, default and range

Without writing anything else, the application has logging, metrics
(`/metrics` for Prometheus), health (`/health`), a crash report, the systemd
watchdog and registration with forge-hub. Next: [controls](02-controls.md).
