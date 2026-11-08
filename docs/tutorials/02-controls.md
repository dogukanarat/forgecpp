# 2. Controls and commands

Controls are declared once in `setup_web()` and appear everywhere: the web
panel (`/forge/`), forgectl and programs on the command link, the console and telnet,
with roles, leases and the audit trail on every channel.

```cpp
void setup_web(forge::web::Dashboard& ui) override {
    auto& drive = ui.group("Drive");
    drive.number("speed_rpm", "Speed", speed_rpm).range(0, 3000, 10).unit("rpm").persist();
    drive.value("running", "Running", running);
    drive.button("start", "Start", [this] { running = true; }).primary();
    drive.button("stop", "Stop", [this] { running = false; }).danger().confirm("Stop the pump?");
    ...
}
```

- A control bound to a member (`speed_rpm`) reads and writes it; one with
  lambdas computes its value (`value("pressure_bar", ..., [this] { ... })`).
- `.range()` is enforced on every channel: setting 9999 rpm is refused with
  400 whether it comes from the panel, forgectl or a script.
- `.persist()` keeps the value across restarts (`[state].file`).
- `.role(forge::Role::admin)` restricts who may change it when `[auth]` is on.
- Everything here runs on the main loop: no locks
  ([threads](../threading.md)).

A **command** takes typed parameters and returns a result:

```cpp
ui.group("Service").command("flush", "Flush", {forge::web::number_param("seconds", 10).range(1, 60)},
    [this](const forge::json& a) {
        running = true;
        const auto s = a["seconds"].get<double>();
        after(std::chrono::milliseconds(static_cast<int>(s * 1000)), [this] { running = false; });
        return forge::json{{"flushing_s", s}};
    });
```

From outside:

```sh
forgectl -e tcp://localhost:5577 set speed_rpm 2400
forgectl -e tcp://localhost:5577 press start
forgectl -e tcp://localhost:5577 call flush seconds=3
forgectl -e tcp://localhost:5577 audit          # who changed what
pump> set speed_rpm 1800                          # the console, same rules
```

The panel shows each group as a card you can move and resize; your own pages
use the same controls with attributes (`<input data-forge-bind="speed_rpm">`,
see the README's web UI section). Next: [configuration, timers and
alarms](03-config-timers-alarms.md).
