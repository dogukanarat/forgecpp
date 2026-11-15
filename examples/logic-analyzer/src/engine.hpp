#pragma once
// The capture engine: one thread reads the words of the source, turns them
// into changes and records them, with a software trigger.
//
//   idle ──start()──> armed ──trigger──> recording ──duration / stop() / edge limit──> idle
//                      │                                     ^
//                      └── trigger "none": straight to ──────┘
//
// While armed, only the last pre_trigger_ms are kept, and the trigger is
// not looked for before that much has been recorded. The capture being
// recorded can be read at any time (SharedCapture): the web UI shows it grow.
// The application main loop never waits for the source.

#include "capture.hpp"
#include "source.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace la {

struct CaptureSettings {
    double sample_hz = 100e6;
    double duration_s = 1.0;           ///< after the trigger (0: until stop())
    std::string trigger = "none";      ///< none | rising | falling | any | pattern
    unsigned trigger_channel = 0;
    std::string pattern = "XXXX";      ///< D0..D3, each 0, 1 or X: the first change into it
    double pre_trigger_ms = 10;        ///< kept before the trigger
    std::size_t max_edges = 4'000'000; ///< memory limit (9 bytes each)
    std::uint32_t glitch_ns = 0;       ///< FPGA glitch filter (0: off)
    double threshold_v = 1.65;         ///< shield comparator threshold
    bool repeat = false;               ///< arm again after each capture
};

enum class State { idle, armed, recording };
const char* state_name(State s);

struct EngineStatus {
    State state = State::idle;
    std::uint64_t capture_id = 0;
    double recorded_s = 0;           ///< length of the capture so far
    double words_per_s = 0, edges_per_s = 0;
    SourceStats source;
    std::string error;               ///< the source failed to start or read
    std::string stop_reason;         ///< of the last capture: duration, stopped, edge limit, error
};

class Engine {
public:
    /// Called on the engine's thread when a capture is complete.
    using OnDone = std::function<void(std::shared_ptr<SharedCapture>)>;

    Engine(std::unique_ptr<Source> source, OnDone on_done);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Starts a new capture (ends the one running first). Thread-safe.
    void start(const CaptureSettings& settings);
    /// Ends the capture now, keeping what it has. Thread-safe.
    void stop();
    /// The capture being recorded, or the last one (nullptr before the first).
    std::shared_ptr<SharedCapture> current() const;
    EngineStatus status() const;
    const Source& source() const { return *source_; }
    /// Channel names for the next captures.
    void set_names(const std::array<std::string, kChannels>& names);

private:
    void run();
    void capture(const CaptureSettings& s);

    std::unique_ptr<Source> source_;
    OnDone on_done_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool quit_ = false;
    bool start_ = false;
    std::atomic<bool> stop_{false};
    CaptureSettings settings_;
    std::shared_ptr<SharedCapture> current_;
    EngineStatus status_;
    std::array<std::string, kChannels> names_{"D0", "D1", "D2", "D3"};
    std::uint64_t next_id_ = 1;
    std::thread thread_;
};

}  // namespace la
