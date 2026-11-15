#pragma once
// logic-analyzer: a four-channel logic analyzer on a Zynq-7000, used from a browser.
//
//   Web UI     /             the analyzer: waveforms, protocol decoders, data table, measurements
//              /forge/       generic control panel (the same capture settings, metrics, logs)
//              /api/...      JSON API of the page (README.md lists it)
//   ZeroMQ     [forge].control  controls and commands (forgectl), [forge].events state, alarms, logs
//   Console    -i or telnet [console].telnet: capture, status, analyzers, decode

#include "decoders.hpp"
#include "engine.hpp"
#include "files.hpp"

#include <forge/forge.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace la {

/// A decoder with its settings, as the user set it up.
struct AnalyzerDef {
    std::string id;       ///< "a1", "a2", ...
    std::string type;     ///< uart | i2c | spi
    std::string name;
    forge::json settings; ///< normalized
    std::string radix = "hex";
    bool enabled = true;
};

/// The decoded frames of every analyzer for one state of one capture.
struct Decoded {
    const SharedCapture* capture = nullptr;
    std::size_t edges = 0;
    bool done = false;
    std::uint64_t version = 0;  ///< of the analyzer list
    std::chrono::steady_clock::time_point at;
    struct Item {
        AnalyzerDef def;
        unsigned channel = 0;
        std::vector<DecodedFrame> frames;
    };
    std::vector<Item> items;
    /// (item, frame) of every frame, by start time.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> order;
};

class LogicAnalyzer : public forge::Application {
public:
    LogicAnalyzer();

    Engine* engine() { return engine_.get(); }
    /// The capture shown: a loaded file, else the engine's current one.
    std::shared_ptr<SharedCapture> shown() const;
    CaptureSettings settings() const;
    std::shared_ptr<const Decoded> decoded();
    CaptureStore* store() { return store_.get(); }

protected:
    void setup_config(forge::config::Schema& schema) override;
    void setup_cli(CLI::App& cli) override;
    void setup_controls(forge::Controls& ui) override;
    void setup_http(forge::http::Server& server) override;
    void setup_console(forge::Console& console) override;
    void on_start() override;
    void on_stop() override;

private:
    void start_capture();
    void stop_capture();
    void captured(std::shared_ptr<SharedCapture> c);  ///< engine thread
    void check_status();
    void load_analyzers();
    void save_analyzers();
    std::string next_id();
    forge::json status_json();
    forge::json analyzers_json() const;
    forge::json analyzer_json(const AnalyzerDef& a) const;
    AnalyzerDef parse_analyzer(const forge::json& body, AnalyzerDef base) const;

    // configuration
    std::string source_kind_ = "sim", scenario_ = "mixed", uio_device_ = "logic-analyzer";
    std::string cli_source_, cli_scenario_;
    std::string captures_dir_ = "captures", analyzers_file_;
    std::size_t keep_captures_ = 20;
    bool start_on_boot_ = false;

    mutable std::mutex mutex_;  // everything below: main loop and HTTP threads
    CaptureSettings settings_;
    std::string rate_label_ = "100 MHz", logic_level_ = "3.3 V";
    std::array<std::string, kChannels> names_{"D0", "D1", "D2", "D3"};
    std::vector<AnalyzerDef> analyzers_;
    std::uint64_t analyzers_version_ = 1;
    unsigned next_analyzer_ = 1;
    std::shared_ptr<SharedCapture> loaded_;
    std::string loaded_file_;
    std::string last_file_;  ///< file of the last capture saved

    std::mutex decode_mutex_;
    std::shared_ptr<const Decoded> decoded_;

    std::unique_ptr<Engine> engine_;
    std::unique_ptr<CaptureStore> store_;
    EngineStatus status_;  // main loop copy for the panel
    std::uint64_t last_lost_ = 0;
};

}  // namespace la
