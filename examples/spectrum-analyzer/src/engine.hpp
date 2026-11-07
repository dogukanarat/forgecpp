#pragma once
// Acquisition + processing engine of the spectrum analyzer.
//
// One thread owns the AD9364 RX path: it opens (and reopens) the device,
// applies settings, retunes per sweep segment, computes spectra and builds
// display frames. The application main loop only exchanges Settings and
// Frames with it, so no hardware call ever runs on the main loop.

#include "backend.hpp"
#include "radio_tap.hpp"
#include "spectrum.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sa {

constexpr std::size_t kTraces = 3;
/// Largest capture read at once (cs16 samples): 1 MB per DMA buffer.
constexpr std::size_t kMaxBufferSamples = std::size_t{1} << 18;
constexpr std::size_t kMarkers = 4;

struct MarkerSettings {
    bool enabled = false;
    double freq_hz = 0;
    bool delta = false;  ///< relative to marker 1 (markers 2-4)
};

struct Settings {
    // frequency / bandwidth
    double center_hz = 2440e6;
    double span_hz = 100e6;
    double rbw_hz = 0;                ///< 0 = auto
    std::size_t vbw_averages = 1;     ///< FFTs averaged per capture (video averaging)
    std::string window = "blackman_harris";
    // amplitude
    std::string gain_mode = "manual";
    double rx_gain_db = 40;
    std::string unit = "dBFS";        ///< dBFS | dBm
    double cal_offset_db = 0;         ///< dBm = dBFS - rx_gain + cal_offset
    // traces
    std::array<std::string, kTraces> trace_modes{"clear_write", "max_hold", "blank"};
    std::size_t trace_average = 10;
    std::string detector = "peak";
    std::size_t points = 1001;
    std::size_t marker_trace = 0;     ///< index into traces
    // markers
    std::array<MarkerSettings, kMarkers> markers{};
    std::size_t active_marker = 0;
    bool peak_track = false;
    bool signal_track = false;
    // measurements / limit
    std::string measurement = "off";  ///< off | channel_power | obw
    double channel_bw_hz = 20e6;
    double obw_percent = 99;
    double peak_threshold_db = -100;  ///< peak table threshold (display unit)
    // sweep
    bool run = true;
    double update_hz = 15;
    std::size_t settle_buffers = 2;   ///< buffers discarded after a retune
};

struct MarkerResult {
    bool enabled = false;
    bool outside = false;  ///< frequency is outside the displayed span (no reading)
    double freq_hz = 0;
    double level = 0;    ///< display unit
    bool delta = false;
    double dfreq_hz = 0, dlevel_db = 0;
};

/// Everything the UI needs for one sweep (levels in the display unit).
struct Frame {
    std::uint64_t seq = 0;
    double start_hz = 0, stop_hz = 0;
    std::size_t points = 0;
    std::string unit;
    double unit_offset_db = 0;        ///< added to dBFS to get the display unit
    std::vector<double> live;         ///< this sweep (for the waterfall)
    std::array<std::vector<double>, kTraces> traces;
    std::array<std::string, kTraces> trace_modes;
    std::array<MarkerResult, kMarkers> markers;
    std::vector<PeakPoint> peaks;     ///< strongest first
    Plan plan;
    double sweep_ms = 0;
    bool overload = false;            ///< ADC clipping in this sweep
    std::optional<double> channel_power;
    std::optional<Occupied> obw;
    std::optional<double> track_center_hz;  ///< signal track request
    double rx_gain_db = 0;
};

struct EngineStatus {
    bool connected = false;
    std::string backend;
    std::string error;
    std::optional<double> temperature_c;
    nlohmann::json device = nlohmann::json::object();
};

class Engine {
public:
    using BackendFactory = std::function<std::unique_ptr<radio::Backend>()>;

    struct Options {
        unsigned rx_channel = 0;   ///< 0 = RX1, 1 = RX2 of the backend
        bool control_gain = true;  ///< apply gain_mode / rx_gain_db (else leave the gain alone)
        std::function<void()> on_thread_start;  ///< runs first on the acquisition thread (RT settings)
    };
    Engine(BackendFactory factory, HardwareLimits limits, Options options);
    Engine(BackendFactory factory, HardwareLimits limits) : Engine(std::move(factory), limits, Options{}) {}
    ~Engine();

    void start(const Settings& initial);
    void stop();

    /// Thread-safe: new settings take effect at the next capture; changes to
    /// frequency, span, RBW, window, detector or points restart the traces.
    void update(const Settings& s);
    /// Thread-safe: clear all traces (max hold, average, ...).
    void reset_traces();
    /// Thread-safe: run one sweep while stopped.
    void single();
    /// Thread-safe: close and reopen the device.
    void reconnect();

    std::shared_ptr<const Frame> latest() const;
    EngineStatus status() const;
    /// Latest raw capture (cs16, realtime spans only) with its rate and LO.
    struct Capture {
        std::vector<std::int16_t> iq;
        double sample_rate_hz = 0;
        double center_hz = 0;
    };
    Capture last_capture() const;

    /// Swept captures run segment by segment; realtime spans in one go.
    static Plan plan_for(const Settings& s, const HardwareLimits& limits);

private:
    void run();
    bool ensure_device();
    void apply_hardware(const Settings& s, bool force);
    bool sweep(const Settings& s, Frame& frame);
    bool tune(double lo_hz, double rate_hz, double rf_bw_hz, std::size_t buffer, std::size_t settle);
    void finish_frame(const Settings& s, Frame& frame, const Bins& bins);

    BackendFactory factory_;
    HardwareLimits limits_;
    Options options_;
    std::size_t read(std::size_t samples);  ///< from the selected channel into iq_

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Settings settings_;
    std::uint64_t settings_version_ = 0;
    bool reset_requested_ = false;
    bool single_requested_ = false;
    bool reconnect_requested_ = false;
    std::shared_ptr<const Frame> latest_;
    EngineStatus status_;
    Capture capture_;

    std::atomic<bool> running_{false};
    std::thread thread_;

    // engine thread state
    std::unique_ptr<radio::Backend> backend_;
    Settings applied_;
    bool hw_applied_ = false;
    double tuned_lo_ = -1, tuned_rate_ = -1, tuned_bw_ = -1;
    std::size_t buffer_size_ = 0;
    bool streaming_ = false;
    std::array<Trace, kTraces> traces_;
    Plan last_plan_;
    std::uint64_t seq_ = 0;
    std::vector<std::int16_t> iq_;
    std::chrono::steady_clock::time_point next_temperature_{};
};

}  // namespace sa
