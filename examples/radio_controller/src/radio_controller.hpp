#pragma once
// radio-controller: AD9364 control and IQ streaming daemon.
//
//   ZeroMQ data plane (cs16 = interleaved int16 I/Q, full scale +-32767):
//     [zmq].rx       PUB   topic "iq": header JSON + samples  (radio -> clients)
//     [zmq].tx       PULL  header JSON + samples                 (clients -> radio)
//     [zmq].rx2      PUB   topic "iq": RX2 blocks (AD9361/AD9363, rx2_enabled): the start
//                          of a DMA buffer, rate limited; the spectrum analyzer's input
//   ZeroMQ control plane:
//     [zmq].control  REP   JSON commands (forge::zmq::serve_application):
//                          {"cmd":"set","id":"rx_lo_mhz","value":2437}
//   Web UI: /  (spectrum, waterfall, controls)   /forge/  (control panel)
//   Console: -i  (controls / set / press / alarms)

#include "backend.hpp"

#include <forge/forge.hpp>
#include <forge/zmq.hpp>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace radio {

class RadioController : public forge::Application {
public:
    RadioController();

    /// Resolved ZeroMQ endpoints (valid after start-up).
    std::string rx_endpoint() const { return rx_endpoint_; }
    std::string rx2_endpoint() const { return rx2_endpoint_; }
    std::string tx_endpoint() const;
    std::string control_endpoint() const;

protected:
    void setup_cli(CLI::App& cli) override;
    void setup_web(forge::web::Dashboard& ui) override;
    void on_start() override;
    void on_stop() override;
    void on_reload(const forge::Config& cfg) override;

private:
    // backend lifecycle (main loop)
    void open_backend();
    void close_backend();
    void device_lost(const std::string& why);
    template <class F>
    void control(const char* what, F&& fn);

    // threads
    struct Endpoints {
        std::string rx, rx2;
    };
    void rx_loop(std::promise<Endpoints> bound);
    void on_tx_frame(forge::zmq::Message&& msg);

    // main loop timers
    void telemetry();
    void update_spectrum();
    void save_state();

    void load_settings(const forge::Config& cfg);
    void mark_dirty() { dirty_ = true; }

    // configuration
    std::string backend_kind_ = "sim";
    std::string uri_;
    std::string cli_backend_, cli_uri_;
    unsigned timeout_ms_ = 2000;
    std::string state_file_;
    std::size_t spectrum_size_ = 1024;
    double temperature_alarm_c_ = 75;
    bool sim_loopback_ = true;
    unsigned sim_loopback_rx_ = 0;
    unsigned sim_rx_channels_ = 1;
    double rx2_tap_hz_ = 20;              ///< RX2 blocks per second (0 = every buffer)
    std::size_t rx2_tap_samples_ = 65536; ///< RX2 block size (at most one DMA buffer)

    // radio state (main loop owns settings_; atomics mirror what threads read)
    Settings settings_;
    std::atomic<double> rx_lo_hz_{0}, rate_hz_{0};
    std::atomic<std::size_t> buffer_size_{65536};
    std::atomic<bool> rx_enabled_{true}, tx_enabled_{false};
    std::atomic<bool> rx2_enabled_{false};
    std::atomic<unsigned> rx_channel_count_{1};
    std::atomic<std::uint64_t> tuning_version_{0};  ///< RX LO / rate / bandwidth changes
    double rx_gain_actual_ = 0;
    std::atomic<double> rx2_gain_actual_{0};
    std::optional<double> temperature_;
    nlohmann::json device_info_ = nlohmann::json::object();
    bool connected_ = false;
    bool dirty_ = false;

    // backend shared with the streaming threads
    std::shared_mutex backend_mutex_;
    std::unique_ptr<Backend> backend_;
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<bool> restart_rx_{false};

    // RX thread
    std::atomic<bool> running_{false};
    std::thread rx_thread_;
    std::string rx_endpoint_;
    std::atomic<std::size_t> subscriptions_{0};
    std::string rx2_endpoint_;
    std::atomic<std::size_t> rx2_subscriptions_{0};
    std::atomic<std::uint64_t> rx2_blocks_{0};
    std::atomic<std::uint64_t> rx_frames_{0}, rx_samples_{0}, rx_overflows_{0};
    std::mutex snapshot_mutex_;
    std::vector<std::int16_t> snapshot_;

    // TX (receiver thread)
    enum class TxMode { idle, streaming, cyclic };
    std::unique_ptr<forge::zmq::Receiver> tx_receiver_;
    std::atomic<TxMode> tx_mode_{TxMode::idle};
    std::atomic<bool> tx_reset_{false};
    std::uint64_t tx_generation_ = 0;   // TX thread only
    std::vector<std::int16_t> tx_pending_;  // TX thread only
    std::atomic<std::uint64_t> tx_frames_{0}, tx_samples_{0}, tx_underflows_{0}, tx_dropped_{0};

    std::unique_ptr<forge::zmq::Server> control_server_;

    // derived telemetry (main loop)
    std::uint64_t last_rx_samples_ = 0, last_rx_overflows_ = 0, last_tx_underflows_ = 0;
    std::chrono::steady_clock::time_point last_telemetry_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_overflow_ = {};
    double rx_rate_msps_ = 0;
    double rx_power_dbfs_ = -200;
    double peak_offset_khz_ = 0;
    double peak_dbfs_ = -200;
};

}  // namespace radio
