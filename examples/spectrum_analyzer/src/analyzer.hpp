#pragma once
// spectrum-analyzer: a swept/realtime spectrum analyzer on the AD9364 RX path.
//
//   Web UI     /            analyzer display (spectrum, waterfall, markers, menus)
//              /forge/      generic control panel
//              /api/frame   latest sweep as JSON, /api/trace.csv, /api/iq.cs16
//   forge.v1   [forge].data     stream "spectrum" (sa.v1.Spectrum, latest only)
//              [forge].control  controls, commands, alarms (forgectl, panels)
//              [forge].events   state, alarms, logs, audit, health, metrics
//   Console    -i or telnet [console].telnet (controls / set / press / peaks / markers)

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
    /// Resolved endpoints (valid after start-up).
    std::string control_endpoint() const { return endpoint("control"); }
    std::string spectrum_endpoint() const { return endpoint("data"); }

protected:
    void setup_config(forge::config::Schema& schema) override;
    void setup_cli(CLI::App& cli) override;
    void setup_web(forge::web::Dashboard& ui) override;
    void setup_http(forge::http::Server& server) override;
    void setup_console(forge::Console& console) override;
    void on_start() override;
    void on_stop() override;

private:
    std::string endpoint(const std::string& name) const;
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
    forge::zmq::Stream* spectrum_ = nullptr;  ///< [forge].data "spectrum"
    std::uint64_t last_seq_ = 0;
    std::chrono::steady_clock::time_point last_overload_{};
    EngineStatus status_;
};

}  // namespace sa
