# 8. Consuming another program's stream in C++

[Tutorial 6](06-streams.md) published the pump's `pressure` stream and read it
from Python. Here a second daemon reads it in C++: the shape every consumer in
the examples has (`examples/spectrum-analyzer/src/radio_tap.cpp` is the full
version, with reconnection and a health report).

```cpp
// monitor.hpp: raises an alarm when the pump's pressure is out of range.
#include <forge/forge.hpp>

class Monitor : public forge::Application {
public:
    Monitor() : Application({"monitor", "1.0.0", "Watches the pump"}) {}

protected:
    void setup_config(forge::config::Schema& s) override {
        s.section("pump", "The pump to watch");
        s.string("pump.data", "tcp://127.0.0.1:5578", "Its [forge].data endpoint");
        s.number("pump.max_bar", 6.0, "Alarm above this pressure").unit("bar");
    }

    void setup_controls(forge::Controls& ui) override {
        auto& g = ui.group("Pump");
        g.value("pressure", "Pressure", pressure_).unit("bar").precision(2);
        g.value("lost", "Messages lost", [this] { return rx_ ? rx_->lost() : 0; });
    }

    void on_start() override {
        const auto cfg = config();
        max_bar_ = cfg.value<double>("pump.max_bar");
        // The receiver has its own thread: hand every message to the main loop,
        // where the application's state lives.
        rx_ = std::make_unique<forge::zmq::Receiver>(
            forge::zmq::Receiver::Kind::sub, cfg.value<std::string>("pump.data"),
            [this](forge::zmq::Message&& m) {
                const auto sample = m.as<pump_sample>();  // checks the header's type
                post([this, sample] { on_sample(sample); });
            },
            forge::zmq::Receiver::Options{.topics = {"pressure"}});
        // No message for 5 s: the pump is gone.
        every(std::chrono::seconds(1), [this] {
            if (now() - last_seen_ > std::chrono::seconds(5)) {
                health().set("pump", forge::HealthState::failed, "no pressure messages");
            }
        });
    }

    void on_stop() override { rx_.reset(); }

private:
    void on_sample(const pump_sample& s) {
        pressure_ = s.bar;
        last_seen_ = now();
        health().set("pump", forge::HealthState::ok);
        if (s.bar > max_bar_) {
            alarms().raise("pump.pressure", forge::Severity::error, std::format("{:.2f} bar", s.bar));
        } else {
            alarms().clear("pump.pressure");
        }
    }

    std::unique_ptr<forge::zmq::Receiver> rx_;
    double pressure_ = 0, max_bar_ = 6;
    forge::Scheduler::Clock::time_point last_seen_{};
};
```

Points worth keeping:

- **The receiver's callback runs on the receiver's thread.** `post()` moves
  the work to the main loop; the controls, alarms and health are touched
  there only ([threading.md](threading.md)).
- **`m.as<T>()` checks the type** named in the stream header and throws when
  the publisher sends something else; `Receiver::lost()` counts the gaps in
  the sequence numbers.
- **Liveness is the consumer's job.** A stream carries no heartbeat
  ([protocol.md](protocol.md#what-is-guaranteed)); a timer that notices
  silence and reports it under health is the pattern.
- **The endpoint is configuration**, declared in the schema with a default
  that matches the device's port plan ([field-notes.md](../field-notes.md#ports-on-a-device)).

The messages: `pump_sample` comes from the pump's `.proto`; the monitor's
CMakeLists.txt compiles the same file with `forge_add_proto(... PROTOS
pump/pump.proto)` (installed by the pump in `share/pump/proto`), so both
programs share one definition.

Test it as in [tutorial 5](05-testing.md): an `AppRunner<Pump>` with
`forge.data=tcp://127.0.0.1:*`, an `AppRunner<Monitor>` with
`-D pump.data=<that endpoint>`, and `wait_until` the monitor's `pressure`
control shows a value.
