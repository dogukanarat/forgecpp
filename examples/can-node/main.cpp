// can-node: a node on a CAN bus.
//
//   heartbeat    every second: id 0x700 + [node].id, [state, uptime (4 bytes, little endian)]
//   temperature  frames from a sensor node: id [node].sensor_id, int16 little endian in 0.01 °C
//                -> the panel, and an alarm above [node].max_temp_c
//   commands     the program's commands over ISO-TP on the same bus ([can]: the
//                framework answers them, as on a serial line)
//
// One worker thread owns the raw socket: it receives the frames and sends the
// heartbeat. What it reads goes to the main loop with post().
//
// The bus may not be there yet (an adapter plugged in later, vcan not set
// up): health reports "bus" as failed and the node tries again every second.
//
// On a PC:
//   sudo modprobe vcan && sudo ip link add vcan0 type vcan && sudo ip link set vcan0 up
//   ./can-node -c can-node.toml --http 8080
//   python3 sensor.py vcan0          # temperature frames, and the node's info over ISO-TP
//   candump vcan0                    # (can-utils) the heartbeats

#include <forge/forge.hpp>

#include <atomic>
#include <cstdint>
#include <optional>

using namespace std::chrono_literals;

namespace node {

inline constexpr std::uint32_t heartbeat_base = 0x700;

/// The heartbeat frame of node `id`.
inline forge::can::Frame heartbeat(std::uint32_t id, std::uint8_t state, std::uint32_t uptime_s) {
    return forge::can::Frame::make(
        heartbeat_base + id, {state, static_cast<std::uint8_t>(uptime_s), static_cast<std::uint8_t>(uptime_s >> 8),
                              static_cast<std::uint8_t>(uptime_s >> 16), static_cast<std::uint8_t>(uptime_s >> 24)});
}

/// °C from a temperature frame (int16, little endian, 0.01 °C); nothing for a frame too short.
inline std::optional<double> temperature(const forge::can::Frame& f) {
    if (f.size < 2 || f.remote) return std::nullopt;
    const auto raw = static_cast<std::int16_t>(f.data[0] | (f.data[1] << 8));
    return raw / 100.0;
}

}  // namespace node

class CanNode : public forge::Application {
public:
    CanNode() : Application({"can-node", "1.0.0", "A node on a CAN bus"}) {}

protected:
    void setup_config(forge::config::Schema& s) override {
        s.section("node", "This node on the bus");
        s.string("node.interface", "vcan0", "CAN interface for the frames");
        s.integer("node.id", 5, "Node id: the heartbeat goes out with 0x700 + id").range(1, 127);
        s.integer("node.sensor_id", 0x185, "Id of the sensor's temperature frames").range(0, 0x7FF);
        s.number("node.max_temp_c", 60.0, "Alarm above this temperature").unit("°C");
    }

    void setup_controls(forge::Controls& ui) override {
        auto& g = ui.group("Bus");
        g.value("temperature", "Temperature", temperature_).unit("°C").precision(2);
        g.value("frames", "Frames received", frames_);
        g.value("heartbeats", "Heartbeats sent", heartbeats_);
        g.number("max_temp_c", "Alarm above", max_temp_c_).unit("°C");
    }

    void on_start() override {
        const auto cfg = config();
        interface_ = cfg.value<std::string>("node.interface");
        node_id_ = static_cast<std::uint32_t>(cfg.value<int>("node.id"));
        sensor_id_ = static_cast<std::uint32_t>(cfg.value<int>("node.sensor_id"));
        max_temp_c_ = cfg.value<double>("node.max_temp_c");
        health().set("bus", forge::HealthState::failed, "not open yet", false);
        worker("can-bus", [this](std::stop_token stop) { run_bus(stop); });
    }

    /// The bus (tests give a socket of their own).
    virtual forge::can::Socket open_bus() {
        return forge::can::Socket(interface_, {.filters = {{sensor_id_, forge::can::standard_mask}}});
    }

private:
    // --- the worker: owns the socket ----------------------------------------------------
    void run_bus(const std::stop_token& stop) {
        std::optional<forge::can::Socket> bus;
        auto next_heartbeat = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            if (!bus) {
                try {
                    bus.emplace(open_bus());
                    post([this] { health().set("bus", forge::HealthState::ok, "on " + interface_, false); });
                } catch (const std::exception& e) {
                    post([this, why = std::string(e.what())] {
                        health().set("bus", forge::HealthState::failed, why, false);
                    });
                    forge::sleep_for(stop, 1s);
                    continue;
                }
            }
            if (std::chrono::steady_clock::now() >= next_heartbeat) {
                next_heartbeat += 1s;
                const auto uptime = static_cast<std::uint32_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(this->uptime()).count());
                if (bus->send(node::heartbeat(node_id_, state_.load(), uptime)).ok()) heartbeats_.fetch_add(1);
            }
            forge::can::Frame f;
            const auto s = bus->receive(f, 100ms);
            if (s.ok()) {
                frames_.fetch_add(1);
                if (f.id == sensor_id_) {
                    if (const auto t = node::temperature(f)) post([this, c = *t] { on_temperature(c); });
                }
            } else if (s.errno_code() != ETIMEDOUT) {  // the interface went down
                post([this, why = std::string(s.message())] {
                    health().set("bus", forge::HealthState::failed, why, false);
                });
                bus.reset();
            }
        }
    }

    // --- the main loop -----------------------------------------------------------------
    void on_temperature(double c) {
        temperature_ = c;
        if (c > max_temp_c_) {
            alarms().raise("node.temperature", forge::Severity::warning, std::format("{:.2f} °C", c));
            state_ = 2;  // in the heartbeat: warning
        } else {
            alarms().clear("node.temperature");
            state_ = 1;  // nominal
        }
    }

    std::string interface_;
    std::uint32_t node_id_ = 5, sensor_id_ = 0x185;
    double max_temp_c_ = 60;  // main loop
    double temperature_ = 0;  // main loop

    std::atomic<std::uint8_t> state_{0};  // 0 starting, 1 nominal, 2 warning: the worker sends it
    std::atomic<std::uint64_t> frames_{0}, heartbeats_{0};
};

FORGE_MAIN(CanNode)
