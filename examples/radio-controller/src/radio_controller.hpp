#pragma once
// radio-controller: AD9364 control and IQ streaming daemon.
//
//   ZeroMQ (radio.iq_block / tx_block + cs16 blob = interleaved int16 I/Q,
//   full scale +-32767):
//     [forge].data   PUB   topic "iq": every buffer of the channels it listens to
//                          ([radio].rx_channels; iq_block.channel)
//                          topics "rx1"/"rx2": blocks of the channels it shares
//                          ([radio].tap_rx), rate limited: e.g. the spectrum analyzer
//     [zmq].tx       PULL  tx_block + samples for [radio].tx_channel
//     [forge].control REP  c2 commands (forgectl, other programs)
//     [forge].events PUB   state, alarms, log, audit, health, metrics
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
    /// [forge].data: topics "iq", "rx1", "rx2".
    std::string data_endpoint() const;
    std::string tx_endpoint() const;
    std::string control_endpoint() const;

protected:
    void setup_cli(CLI::App& cli) override;
    void setup_config(forge::config::Schema& schema) override;
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
    void rx_loop();
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
    SimOptions sim_;
    // channel roles from [radio] (bit 0 = channel 1, bit 1 = channel 2)
    unsigned rx_mask_ = Backend::kRx1;    ///< RX channels it listens to (IQ stream)
    unsigned tap_mask_ = 0;               ///< RX channels it shares on [zmq].tap
    unsigned tx_channel_ = 0;             ///< TX channel it drives (0 = TX1)
    double tap_hz_ = 20;                  ///< tap blocks per second (0 = every buffer)
    std::size_t tap_samples_ = 65536;     ///< tap block size (at most one DMA buffer)

    // radio state (main loop owns settings_; atomics mirror what threads read)
    Settings settings_;
    std::atomic<double> rx_lo_hz_{0}, rate_hz_{0};
    std::atomic<std::size_t> buffer_size_{65536};
    std::atomic<bool> rx_enabled_{true}, tx_enabled_{false};
    std::atomic<unsigned> hw_rx_channels_{1}, hw_tx_channels_{1};
    std::atomic<std::uint64_t> tuning_version_{0};  ///< RX LO / rate / bandwidth changes
    std::atomic<double> rx_gain_actual_[2] = {0.0, 0.0};
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
    forge::zmq::Stream* iq_stream_ = nullptr;
    forge::zmq::Stream* tap_stream_[2] = {nullptr, nullptr};
    std::atomic<std::size_t> subscriptions_{0};
    std::atomic<std::size_t> tap_subscriptions_{0};
    std::atomic<std::uint64_t> tap_blocks_{0};
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
    std::atomic<std::uint64_t> tx_frames_{0}, tx_samples_{0}, tx_underflows_{0}, tx_dropped_{0}, tx_late_{0};
    std::atomic<int> tx_max_age_ms_{0};


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
