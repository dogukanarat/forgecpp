#include "engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace sa {
namespace {

using Clock = std::chrono::steady_clock;

// Settings whose change makes the existing traces meaningless.
bool structural_change(const Settings& a, const Settings& b) {
    return a.center_hz != b.center_hz || a.span_hz != b.span_hz || a.rbw_hz != b.rbw_hz || a.window != b.window ||
           a.detector != b.detector || a.points != b.points || a.vbw_averages != b.vbw_averages;
}

}  // namespace

Engine::Engine(BackendFactory factory, HardwareLimits limits, Options options)
    : factory_(std::move(factory)), limits_(limits), options_(options) {}

std::size_t Engine::read(std::size_t samples) {
    return options_.rx_channel == 0 ? backend_->read_rx(iq_.data(), nullptr, samples)
                                    : backend_->read_rx(nullptr, iq_.data(), samples);
}

Engine::~Engine() { stop(); }

Plan Engine::plan_for(const Settings& s, const HardwareLimits& limits) {
    return make_plan(s.center_hz, s.span_hz, s.rbw_hz, radio::dsp::parse_window(s.window), limits);
}

void Engine::start(const Settings& initial) {
    {
        std::lock_guard lock(mutex_);
        settings_ = initial;
        ++settings_version_;
    }
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void Engine::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard lock(mutex_);
        if (backend_) backend_->cancel_rx();  // unblock a pending read
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Engine::update(const Settings& s) {
    {
        std::lock_guard lock(mutex_);
        settings_ = s;
        ++settings_version_;
    }
    cv_.notify_all();
}

void Engine::reset_traces() {
    {
        std::lock_guard lock(mutex_);
        reset_requested_ = true;
    }
    cv_.notify_all();
}

void Engine::single() {
    {
        std::lock_guard lock(mutex_);
        single_requested_ = true;
    }
    cv_.notify_all();
}

void Engine::reconnect() {
    {
        std::lock_guard lock(mutex_);
        reconnect_requested_ = true;
        if (backend_) backend_->cancel_rx();
    }
    cv_.notify_all();
}

std::shared_ptr<const Frame> Engine::latest() const {
    std::lock_guard lock(mutex_);
    return latest_;
}

EngineStatus Engine::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

Engine::Capture Engine::last_capture() const {
    std::lock_guard lock(mutex_);
    return capture_;
}

// ---------------------------------------------------------------------------
// engine thread

bool Engine::ensure_device() {
    if (backend_) return true;
    try {
        auto b = factory_();
        auto info = b->describe();
        const auto name = b->name();
        std::lock_guard lock(mutex_);
        backend_ = std::move(b);
        status_.connected = true;
        status_.backend = name;
        status_.device = std::move(info);
        status_.error.clear();
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        status_.connected = false;
        status_.error = e.what();
        return false;
    }
    hw_applied_ = false;
    tuned_lo_ = tuned_rate_ = tuned_bw_ = -1;
    streaming_ = false;
    return true;
}

void Engine::apply_hardware(const Settings& s, bool force) {
    if (!options_.control_gain) return;
    const unsigned ch = options_.rx_channel;
    if (force || s.gain_mode != applied_.gain_mode) backend_->set_gain_mode(s.gain_mode, ch);
    if (s.gain_mode == "manual" &&
        (force || s.gain_mode != applied_.gain_mode || s.rx_gain_db != applied_.rx_gain_db)) {
        backend_->set_rx_gain(s.rx_gain_db, ch);
    }
}

bool Engine::tune(double lo, double rate, double rf_bw, std::size_t buffer, std::size_t settle) {
    bool changed = false;
    if (rate != tuned_rate_) {
        backend_->set_sample_rate(rate);
        tuned_rate_ = rate;
        changed = true;
    }
    if (rf_bw != tuned_bw_) {
        backend_->set_rx_bandwidth(rf_bw);
        tuned_bw_ = rf_bw;
        changed = true;
    }
    if (lo != tuned_lo_) {
        backend_->set_rx_lo(lo);
        tuned_lo_ = lo;
        changed = true;
    }
    if (!streaming_ || buffer != buffer_size_) {
        if (streaming_) backend_->stop_rx();
        backend_->start_rx(buffer, 1u << options_.rx_channel);
        buffer_size_ = buffer;
        streaming_ = true;
        iq_.resize(2 * buffer);
        changed = true;
    }
    // Buffers already queued in the DMA were captured with the old settings.
    if (changed) {
        for (std::size_t i = 0; i < settle; ++i) {
            if (read(buffer) == 0) return false;
        }
    }
    return true;
}

bool Engine::sweep(const Settings& s, Frame& frame) {
    const auto window = radio::dsp::parse_window(s.window);
    auto* fixed = dynamic_cast<FixedTuning*>(backend_.get());
    Tuning tuning;
    Plan plan;
    if (fixed) {
        tuning = fixed->tuning();  // radio-controller decides LO and rate
        if (tuning.rate_hz <= 0 || tuning.block_samples == 0) return false;  // cancelled
        plan = make_fixed_plan(s.center_hz, s.span_hz, s.rbw_hz, window, tuning.lo_hz, tuning.rate_hz,
                               tuning.block_samples, limits_);
    } else {
        plan = plan_for(s, limits_);
    }

    static thread_local std::size_t fft_n = 0;
    static thread_local radio::dsp::Window fft_window = radio::dsp::Window::rectangular;
    static thread_local std::unique_ptr<radio::dsp::Fft> fft;
    static thread_local radio::dsp::WindowInfo window_info;
    if (!fft || fft_n != plan.fft_size || fft_window != window) {
        fft = std::make_unique<radio::dsp::Fft>(plan.fft_size);
        window_info = radio::dsp::make_window(window, plan.fft_size);
        fft_n = plan.fft_size;
        fft_window = window;
    }

    // Video averaging needs `ffts` FFTs per segment. They are read in chunks
    // of at most kMaxBufferSamples, so a DMA buffer stays small (the kernel
    // keeps several of them in CMA memory) whatever the RBW and averaging.
    const std::size_t ffts = std::clamp<std::size_t>(s.vbw_averages, 1, 256);
    const std::size_t chunk_ffts = std::clamp<std::size_t>(kMaxBufferSamples / plan.fft_size, 1, ffts);
    const std::size_t buffer = fixed ? tuning.block_samples : std::max<std::size_t>(plan.fft_size * chunk_ffts, 1024);
    std::uint64_t version;
    {
        std::lock_guard lock(mutex_);
        version = settings_version_;
    }

    Bins bins;
    std::vector<double> spectrum, part;
    bool overload = false;
    std::size_t last_ffts = 0;
    for (const auto& seg : plan.segments) {
        {
            std::lock_guard lock(mutex_);
            if (!running_ || settings_version_ != version || reconnect_requested_)
                return false;  // restart with new settings
        }
        if (fixed) {
            if (!streaming_ || buffer != buffer_size_) {
                backend_->start_rx(buffer);  // the tap's only channel
                buffer_size_ = buffer;
                streaming_ = true;
                iq_.resize(2 * buffer);
            }
        } else if (!tune(seg.lo_hz, plan.sample_rate_hz, plan.rf_bandwidth_hz, buffer, s.settle_buffers)) {
            return false;
        }

        spectrum.assign(plan.fft_size, 0.0);
        std::size_t got = 0;
        while (got < ffts) {
            const std::size_t n = fixed ? backend_->read_rx(iq_.data(), buffer) : read(buffer);
            if (n == 0) return false;  // cancelled
            if (fixed && fixed->last_read() != tuning) return false;  // retuned meanwhile: plan again
            const std::size_t k = std::min(n / plan.fft_size, ffts - got);
            if (k == 0) throw radio::DeviceError("capture shorter than one FFT");
            overload = overload || radio::dsp::peak_magnitude(iq_.data(), plan.fft_size * k) >= 0.99;
            radio::dsp::power_spectrum(iq_.data(), k, *fft, window_info, part);
            for (std::size_t i = 0; i < spectrum.size(); ++i) spectrum[i] += part[i] * static_cast<double>(k);
            got += k;
            last_ffts = k;
        }
        for (auto& v : spectrum) v /= static_cast<double>(got);
        append_bins(bins, spectrum, seg.lo_hz, plan.sample_rate_hz, seg.from_hz, seg.to_hz);
    }
    if (!plan.swept) {
        std::lock_guard lock(mutex_);
        capture_.iq.assign(iq_.begin(), iq_.begin() + static_cast<std::ptrdiff_t>(2 * plan.fft_size * last_ffts));
        capture_.sample_rate_hz = plan.sample_rate_hz;
        capture_.center_hz = plan.segments.front().lo_hz;
    }
    // A different window or resolution (e.g. radio-controller retuned) makes the held traces meaningless.
    if (plan.start_hz != last_plan_.start_hz || plan.stop_hz != last_plan_.stop_hz ||
        plan.fft_size != last_plan_.fft_size || plan.sample_rate_hz != last_plan_.sample_rate_hz) {
        for (auto& t : traces_) t.reset();
        last_plan_ = plan;
    }
    frame.plan = plan;
    frame.overload = overload;
    finish_frame(s, frame, bins);
    return true;
}

void Engine::finish_frame(const Settings& s, Frame& frame, const Bins& bins) {
    const auto& plan = frame.plan;
    frame.seq = ++seq_;
    frame.start_hz = plan.start_hz;
    frame.stop_hz = plan.stop_hz;
    frame.points = std::clamp<std::size_t>(s.points, 11, 20001);

    const auto live = to_display(bins, plan.start_hz, plan.stop_hz, frame.points, parse_detector(s.detector));
    for (std::size_t i = 0; i < kTraces; ++i) traces_[i].update(live, s.trace_average);

    // Display unit: dBFS, or dBm = dBFS - RX gain + calibration offset.
    try {
        frame.rx_gain_db = backend_->rx_gain(dynamic_cast<FixedTuning*>(backend_.get()) ? 0 : options_.rx_channel);
    } catch (const radio::DeviceError&) {
        frame.rx_gain_db = s.rx_gain_db;
    }
    frame.unit = s.unit;
    frame.unit_offset_db = s.unit == "dBm" ? s.cal_offset_db - frame.rx_gain_db : 0.0;
    const double off = frame.unit_offset_db;
    auto shifted = [off](const std::vector<double>& v) {
        std::vector<double> out(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) out[i] = v[i] + off;
        return out;
    };
    frame.live = shifted(live);
    for (std::size_t i = 0; i < kTraces; ++i) {
        frame.trace_modes[i] = s.trace_modes[i];
        if (traces_[i].visible()) frame.traces[i] = shifted(traces_[i].db());
    }

    // Markers and peaks work on the marker trace (or the live sweep if it is blank).
    const std::size_t mt = std::min(s.marker_trace, kTraces - 1);
    const auto& mdb = traces_[mt].visible() ? traces_[mt].db() : live;
    frame.peaks = find_peaks(mdb, plan.start_hz, plan.stop_hz, s.peak_threshold_db - off, 6.0, 10);
    for (auto& p : frame.peaks) p.db += off;

    for (std::size_t i = 0; i < kMarkers; ++i) {
        const auto& m = s.markers[i];
        auto& r = frame.markers[i];
        r.enabled = m.enabled;
        if (!m.enabled) continue;
        r.freq_hz = m.freq_hz;
        if (s.peak_track && i == s.active_marker && !frame.peaks.empty()) r.freq_hz = frame.peaks.front().freq_hz;
        // A marker outside the span keeps its frequency but has no reading
        // (clamping it to the edge would show a level that is not there).
        const double edge = (plan.stop_hz - plan.start_hz) * 1e-9;
        if (r.freq_hz < plan.start_hz - edge || r.freq_hz > plan.stop_hz + edge) {
            r.outside = true;
            continue;
        }
        r.level = mdb[point_index(r.freq_hz, plan.start_hz, plan.stop_hz, frame.points)] + off;
    }
    for (std::size_t i = 1; i < kMarkers; ++i) {
        auto& r = frame.markers[i];
        const auto& ref = frame.markers[0];
        if (r.enabled && !r.outside && s.markers[i].delta && ref.enabled && !ref.outside) {
            r.delta = true;
            r.dfreq_hz = r.freq_hz - frame.markers[0].freq_hz;
            r.dlevel_db = r.level - frame.markers[0].level;
        }
    }

    if (s.signal_track && !frame.peaks.empty()) {
        const double f = frame.peaks.front().freq_hz;
        if (std::abs(f - s.center_hz) > s.span_hz * 0.02) frame.track_center_hz = f;
    }

    if (s.measurement == "channel_power") {
        const double half = s.channel_bw_hz / 2;
        frame.channel_power = channel_power_db(bins, s.center_hz - half, s.center_hz + half, plan.enbw_bins) + off;
    } else if (s.measurement == "obw") {
        frame.obw = occupied_bandwidth(bins, std::clamp(s.obw_percent, 1.0, 99.99) / 100.0);
    }
}

void Engine::run() {
    if (options_.on_thread_start) options_.on_thread_start();
    std::uint64_t seen = ~0ull;
    Settings s;
    while (running_) {
        bool do_reset = false, do_single = false, do_reconnect = false;
        std::uint64_t version;
        {
            std::lock_guard lock(mutex_);
            s = settings_;
            version = settings_version_;
            do_reset = std::exchange(reset_requested_, false);
            do_single = single_requested_;
            do_reconnect = std::exchange(reconnect_requested_, false);
        }

        if (do_reconnect && backend_) {
            std::lock_guard lock(mutex_);
            backend_.reset();
            status_.connected = false;
        }
        if (!ensure_device()) {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(2), [this] { return !running_ || reconnect_requested_; });
            continue;
        }

        try {
            if (version != seen) {
                if (!hw_applied_ || structural_change(s, applied_)) {
                    for (auto& t : traces_) t.reset();
                }
                for (std::size_t i = 0; i < kTraces; ++i) traces_[i].set_mode(parse_trace_mode(s.trace_modes[i]));
                apply_hardware(s, !hw_applied_);
                applied_ = s;
                hw_applied_ = true;
                seen = version;
            }
            if (do_reset) {
                for (auto& t : traces_) t.reset();
            }

            if (Clock::now() >= next_temperature_) {
                auto t = backend_->temperature_c();
                std::lock_guard lock(mutex_);
                status_.temperature_c = t;
                next_temperature_ = Clock::now() + std::chrono::seconds(2);
            }

            if (!s.run && !do_single) {
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(100), [this, version] {
                    return !running_ || settings_version_ != version || single_requested_ || reconnect_requested_;
                });
                continue;
            }

            const auto t0 = Clock::now();
            auto frame = std::make_shared<Frame>();
            if (!sweep(s, *frame)) continue;  // settings changed or cancelled: start over
            frame->sweep_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            {
                std::lock_guard lock(mutex_);
                latest_ = std::move(frame);
                if (do_single) single_requested_ = false;
                status_.error.clear();
            }

            // Realtime spans: limit the update rate (swept spans take their own time).
            const auto period = std::chrono::duration<double>(1.0 / std::clamp(s.update_hz, 0.5, 60.0));
            const auto next = t0 + std::chrono::duration_cast<Clock::duration>(period);
            std::unique_lock lock(mutex_);
            cv_.wait_until(lock, next, [this, version] {
                return !running_ || settings_version_ != version || reset_requested_ || reconnect_requested_;
            });
        } catch (const std::exception& e) {
            // Device gone or rejected the request: report and reopen.
            std::lock_guard lock(mutex_);
            status_.error = e.what();
            status_.connected = false;
            backend_.reset();
        }
    }
    std::lock_guard lock(mutex_);
    backend_.reset();
}

}  // namespace sa
