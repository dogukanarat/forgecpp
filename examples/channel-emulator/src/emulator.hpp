#pragma once
// channel-emulator: radio channel impairments in the TX path.
//
//   qam-modem (or any TX client) --PUSH TxBlock--> [emulator].input (PULL, binds 5586)
//        --> impairments (examples/common/channel) --> PUSH --> radio-controller [zmq].tx (5556)
//
// Applied before the DAC, so every RX behind the splitter sees the impaired
// signal: the modem on RX1, the spectrum analyzer on RX2. The emulator never
// touches the radio; it only reads radio-controller's sample rate (Hz-based
// settings need it). Sample count and backpressure pass through unchanged:
// a slow radio slows the emulator, which slows the sender.
//
// Threads: the ZeroMQ receiver thread processes and forwards every block;
// the controls (main loop) hand new settings over under a mutex.

#include "channel.hpp"

#include <forge/forge.hpp>
#include <forge/zmq.hpp>

#include <atomic>
#include <chrono>
#include <vector>
#include <memory>
#include <mutex>
#include <string>

namespace emulator {

class ChannelEmulator : public forge::Application {
public:
    ChannelEmulator();

    /// Resolved input endpoint (useful with "tcp://127.0.0.1:*" in tests).
    std::string input_endpoint() const;

protected:
    void setup_config(forge::config::Schema& s) override;
    void setup_web(forge::web::Dashboard& ui) override;
    void on_start() override;
    void on_stop() override;

private:
    // main loop
    void apply(const std::string& preset_name);  ///< "custom" = keep the current settings
    void changed();                              ///< a control changed settings_: hand them over
    void read_sample_rate();
    void tick();

    // receiver thread
    void on_block(forge::zmq::Message&& m);

    template <class T>
    void bind(forge::web::Group& g, const std::string& id, const std::string& label, T channel::Settings::*field,
              double min = 0, double max = 0, double step = 0, const std::string& unit = {});

    // main loop state
    channel::Settings settings_;
    std::string preset_ = "clean";
    std::string paths_text_;
    double noise_bandwidth_hz_ = 0;
    std::string radio_control_;
    bool reading_rate_ = false;
    std::string rate_source_ = "configuration";
    std::chrono::steady_clock::time_point last_tick_{};
    std::uint64_t last_clipped_ = 0, last_samples_ = 0;
    double msps_ = 0;

    // handed to the receiver thread
    std::mutex pending_mutex_;
    channel::Settings pending_;
    std::atomic<unsigned> version_{0};
    std::atomic<double> rate_hz_{3e6};

    // receiver thread
    std::unique_ptr<channel::Channel> channel_;
    unsigned applied_version_ = 0;
    std::unique_ptr<forge::zmq::Sender> out_;
    std::unique_ptr<forge::zmq::Receiver> in_;
    std::atomic<bool> running_{false};

    // statistics (receiver thread writes)
    std::atomic<std::uint64_t> blocks_{0}, samples_{0}, clipped_{0}, cyclic_{0}, dropped_{0}, timing_resets_{0},
        timing_forced_{0}, waits_{0};
    std::atomic<double> signal_dbfs_{-200}, cpu_{0};
};

/// "0:0:0, 0.5:-3:90" (delay_us:gain_db[:phase_deg], ...) <-> paths. Throws std::invalid_argument.
std::vector<channel::Path> parse_paths(const std::string& text);
std::string format_paths(const std::vector<channel::Path>& paths);

}  // namespace emulator
