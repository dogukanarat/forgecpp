# 3. Configuration, timers and alarms

**Configuration** is declared with types, ranges and documentation; the
default is written once, here:

```cpp
void setup_config(forge::config::Schema& s) override {
    s.section("pump", "The pump");
    s.number("pump.limit_bar", 8.0, "Overpressure alarm limit").range(1, 20).unit("bar");
    s.integer("pump.sample_ms", 100, "Pressure sampling period").range(10, 10000).unit("ms");
    s.integer("pump.alarm_after_s", 3, "Overpressure this long raises the alarm").range(0, 600).unit("s");
}
```

and read with `value<T>()`:

```cpp
void on_start() override {
    const auto cfg = config();
    limit_bar = cfg.value<double>("pump.limit_bar");
    every(std::chrono::milliseconds(cfg.value<int>("pump.sample_ms")), [this] { sample(); });
}
```

A wrong type or a value out of range stops the start with a message naming
the key; a typo (`limit_bra`) is a warning with a suggestion. Overrides:
`-D pump.limit_bar=9`; reload: SIGHUP or `reload` in the console
(`on_reload()`).

**Timers** (`every`, `after`) run on the main loop. Code that depends on time
reads `now()`, the application's clock, so a test can control it
([testing](05-testing.md)).

**Alarms** are what an operator must see: raised, acknowledged, cleared, with
a bell and a banner on every page, the forge-hub dashboard and forge.v1
events. Raising an active alarm only updates it. With hysteresis:

```cpp
if (pressure_bar > limit_bar) {
    if (!over_since_) over_since_ = now();
    if (now() - *over_since_ >= alarm_after_) {
        alarms().raise("pump.overpressure", forge::Severity::error,
                       std::format("Pressure {:.1f} bar above {:.1f} bar", pressure_bar, limit_bar));
    }
} else if (pressure_bar < limit_bar * 0.95) {
    over_since_.reset();
    alarms().clear("pump.overpressure");
}
```

Use `notify_web()` for messages that need no action, and `health()` for the
state of a component (it drives `/health` and the watchdog). Next:
[threads](04-threads.md).
