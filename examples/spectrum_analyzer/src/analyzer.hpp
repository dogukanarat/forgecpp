#pragma once
// spectrum-analyzer: a swept/realtime spectrum analyzer on the AD9364 RX path.
//
//   Web UI     /            analyzer display (spectrum, waterfall, markers, menus)
//              /forge/      generic control panel
//              /api/frame   latest sweep as JSON, /api/trace.csv, /api/iq.cs16
//   ZeroMQ     [zmq].spectrum  PUB "spectrum": header JSON + float32 live trace
//              [zmq].control   REP JSON commands (same controls as the web UI)
//   Console    -i  (controls / set / press / peaks / markers)

#include "engine.hpp"

#include <forge/forge.hpp>
#include <forge/zmq.hpp>

#include <memory>
#include <string>

namespace sa {

class Analyzer : public forge::Application {
public:
    Analyzer();

    /// Latest frame (for tests and HTTP handlers).
    std::shared_ptr<const Frame> latest() const { return engine_ ? engine_->latest() : nullptr; }
    std::string control_endpoint() const { return control_ ? control_->endpoint() : ""; }
    std::string spectrum_endpoint() const { return spectrum_endpoint_; }

protected:
    void setup_cli(CLI::App& cli) override;
    void setup_web(forge::web::Dashboard& ui) override;
    void setup_http(forge::http::Server& server) override;
    void setup_console(forge::Console& console) override;
    void on_start() override;
    void on_stop() override;

private:
    void load(const forge::Config& cfg);
    void changed();                       ///< push settings to the engine, persist later
    void set_frequency(double center_hz, double span_hz);
    void publish_frame();
    void check_status();
    void save_state();
    forge::json frame_json(const Frame& f) const;
    std::shared_ptr<const Frame> require_frame() const;

    // marker commands
    MarkerSettings& active();
    void require_gain_control() const;
    void peak_search();
    void next_peak(int direction);  ///< 0 = next lower, -1 = left, +1 = right

    std::string backend_kind_ = "sim";
    std::string uri_;
    std::string cli_backend_, cli_uri_;
    RadioTapOptions tap_;           ///< backend "radio": a channel radio-controller shares
    unsigned channel_ = 1;          ///< RX channel listened to (1 or 2)
    bool control_gain_ = true;      ///< the analyzer sets that channel's gain
    unsigned timeout_ms_ = 2000;
    std::string state_file_;
    HardwareLimits limits_;

    Settings settings_;
    double ref_level_ = 0;      ///< display: top line (display unit)
    double scale_db_ = 10;      ///< display: dB per division
    bool limit_on_ = false;
    double limit_level_ = -40;  ///< display unit
    bool limit_alarm_ = false, overload_alarm_ = false;
    bool dirty_ = false;

    std::unique_ptr<Engine> engine_;
    std::unique_ptr<forge::zmq::Server> control_;
    std::unique_ptr<forge::zmq::Sender> spectrum_pub_;  ///< main loop only
    std::string spectrum_endpoint_;
    std::uint64_t last_seq_ = 0;
    std::chrono::steady_clock::time_point last_overload_{};
    EngineStatus status_;
};

}  // namespace sa
