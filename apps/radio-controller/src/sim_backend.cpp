// Simulated AD9364 for development and tests without hardware.
//
// The "air" contains a few carriers at fixed RF frequencies. Whatever falls
// inside the tuned band (LO +- min(rate, bandwidth)/2) shows up at its offset
// from the LO, scaled by the RX gain, on top of a noise floor that also follows
// the gain. Samples are paced in real time like the real device: a reader that
// falls behind by more than a buffer gets an overflow. With loopback enabled,
// transmitted samples are mixed into RX, shifted by (TX LO - RX LO).

#include "backend.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <mutex>
#include <random>
#include <thread>

namespace radio {
namespace {

using Clock = std::chrono::steady_clock;
using cfloat = std::complex<float>;

struct Carrier {
    double hz;
    double dbfs_at_0db;  ///< level at 0 dB RX gain
};

// A small RF environment: ISM-band beacons and a couple of narrowband signals.
const std::vector<Carrier>& carriers() {
    static const std::vector<Carrier> c{
        {433.92e6, -42}, {868.3e6, -50}, {915.0e6, -47},  {1575.42e6, -70},
        {2412.0e6, -46}, {2418.5e6, -58}, {2437.0e6, -52}, {5800.0e6, -55},
    };
    return c;
}

constexpr double kNoiseDbfsAt0dB = -100.0;
constexpr double kLoopbackCouplingDb = -20.0;  ///< TX output -> RX input in the simulated setup

double db_to_amp(double db) { return std::pow(10.0, db / 20.0); }

class SimBackend final : public Backend {
public:
    explicit SimBackend(SimOptions o) : options_(o), rng_(o.seed) {
        std::normal_distribution<float> n(0.0f, std::sqrt(0.5f));
        noise_.resize(1 << 16);
        for (auto& v : noise_) v = {n(rng_), n(rng_)};
        phases_.assign(carriers().size(), {1.0, 0.0});
    }

    std::string name() const override { return "sim"; }

    nlohmann::json describe() const override {
        return {{"backend", "sim"},
                {"model", "Simulated AD9364"},
                {"loopback", options_.loopback},
                {"carriers_mhz", [] {
                     nlohmann::json a = nlohmann::json::array();
                     for (const auto& c : carriers()) a.push_back(c.hz / 1e6);
                     return a;
                 }()}};
    }

    // --- control -------------------------------------------------------------
    void set_rx_lo(double hz) override { set(&State::rx_lo, hz); }
    void set_tx_lo(double hz) override { set(&State::tx_lo, hz); }
    void set_sample_rate(double hz) override { set(&State::rate, hz); }
    void set_rx_bandwidth(double hz) override { set(&State::rx_bw, hz); }
    void set_tx_bandwidth(double hz) override { set(&State::tx_bw, hz); }
    void set_gain_mode(const std::string& mode) override {
        std::lock_guard lock(mutex_);
        state_.gain_mode = mode;
    }
    void set_rx_gain(double db) override {
        std::lock_guard lock(mutex_);
        if (state_.gain_mode != "manual") throw DeviceError("RX gain can only be set in manual gain mode");
        state_.gain = db;
    }
    void set_tx_attenuation(double db) override { set(&State::tx_atten, std::round(db * 4) / 4); }
    double rx_gain() override {
        std::lock_guard lock(mutex_);
        return state_.gain;
    }
    std::optional<double> temperature_c() override {
        const auto t = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
        return 41.5 + 1.5 * std::sin(t / 60.0);
    }

    // --- RX ------------------------------------------------------------------------
    void start_rx(std::size_t samples) override {
        rx_samples_ = std::max<std::size_t>(samples, 64);
        cancelled_ = false;
        deadline_ = Clock::now();
    }

    std::size_t read_rx(std::int16_t* iq, std::size_t max_samples) override {
        if (cancelled_) return 0;
        const State s = snapshot();
        const std::size_t n = std::min(max_samples, rx_samples_);
        const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(n / s.rate));

        // Real-time pacing; a consumer that is late by a whole buffer lost samples.
        deadline_ += period;
        const auto now = Clock::now();
        if (now > deadline_ + period) {
            rx_overflow_ = true;
            deadline_ = now;
        }
        while (Clock::now() < deadline_) {
            if (cancelled_) return 0;
            std::this_thread::sleep_for(std::min<Clock::duration>(deadline_ - Clock::now(), std::chrono::milliseconds(20)));
        }

        generate(s, iq, n);
        return n;
    }

    void stop_rx() override {}
    void cancel_rx() override { cancelled_ = true; }
    bool take_rx_overflow() override { return rx_overflow_.exchange(false); }

    // --- TX ------------------------------------------------------------------------
    void start_tx(std::size_t samples, bool cyclic) override {
        std::lock_guard lock(tx_mutex_);
        tx_samples_ = samples;
        tx_cyclic_ = cyclic;
        tx_ring_.clear();
        tx_head_ = 0;
        tx_pattern_.clear();
        tx_pos_ = 0;
        tx_active_ = true;
        tx_deadline_ = Clock::now();
    }

    void write_tx(const std::int16_t* iq, std::size_t samples) override {
        const double rate = snapshot().rate;
        {
            std::lock_guard lock(tx_mutex_);
            if (!tx_active_) throw DeviceError("TX is not started");
            if (tx_cyclic_) {
                tx_pattern_.assign(samples, {});
                for (std::size_t i = 0; i < samples; ++i) tx_pattern_[i] = {iq[2 * i] / 32768.0f, iq[2 * i + 1] / 32768.0f};
                tx_pos_ = 0;
                return;
            }
            // A streaming writer that pauses longer than one buffer leaves a gap.
            const auto now = Clock::now();
            const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(samples / rate));
            if (tx_deadline_ + period < now && tx_writes_ > 0) tx_underflow_ = true;
            if (tx_deadline_ < now) tx_deadline_ = now;
            tx_deadline_ += period;
            // Compact the queue, append, keep at most a few buffers (oldest dropped).
            tx_ring_.erase(tx_ring_.begin(), tx_ring_.begin() + static_cast<std::ptrdiff_t>(tx_head_));
            tx_head_ = 0;
            const std::size_t old = tx_ring_.size();
            tx_ring_.resize(old + samples);
            for (std::size_t i = 0; i < samples; ++i) tx_ring_[old + i] = {iq[2 * i] / 32768.0f, iq[2 * i + 1] / 32768.0f};
            const std::size_t cap = 8 * std::max<std::size_t>(tx_samples_, samples);
            if (tx_ring_.size() > cap) tx_head_ = tx_ring_.size() - cap;
            ++tx_writes_;
        }
        // Like the hardware, writes block while the queue is a buffer ahead.
        const auto wait_until = tx_deadline_ - std::chrono::duration_cast<Clock::duration>(
                                                  std::chrono::duration<double>(samples / rate));
        std::this_thread::sleep_until(wait_until);
    }

    void stop_tx() override {
        std::lock_guard lock(tx_mutex_);
        tx_active_ = false;
        tx_ring_.clear();
        tx_head_ = 0;
        tx_pattern_.clear();
        tx_writes_ = 0;
    }

    bool take_tx_underflow() override { return tx_underflow_.exchange(false); }

private:
    struct State {
        double rx_lo = 2400e6, tx_lo = 2400e6, rate = 30.72e6, rx_bw = 18e6, tx_bw = 18e6;
        double gain = 30, tx_atten = 10;
        std::string gain_mode = "slow_attack";
    };

    void set(double State::*field, double value) {
        std::lock_guard lock(mutex_);
        state_.*field = value;
    }

    State snapshot() {
        std::lock_guard lock(mutex_);
        return state_;
    }

    void generate(const State& s, std::int16_t* iq, std::size_t n) {
        buffer_.assign(n, {});
        const double half_band = std::min(s.rate, s.rx_bw) / 2.0;
        const double gain_amp = db_to_amp(s.gain);

        // noise (random start in a precomputed table)
        const float noise_amp = static_cast<float>(db_to_amp(kNoiseDbfsAt0dB) * gain_amp);
        std::size_t idx = std::uniform_int_distribution<std::size_t>(0, noise_.size() - 1)(rng_);
        for (auto& v : buffer_) {
            v = noise_[idx] * noise_amp;
            idx = (idx + 1) & (noise_.size() - 1);
        }

        // carriers inside the band, as complex rotators (no sin/cos per sample)
        for (std::size_t c = 0; c < carriers().size(); ++c) {
            const double offset = carriers()[c].hz - s.rx_lo;
            if (std::abs(offset) >= half_band) continue;
            const double amp = db_to_amp(carriers()[c].dbfs_at_0db) * gain_amp;
            const double w = 2 * M_PI * offset / s.rate;
            const std::complex<double> step(std::cos(w), std::sin(w));
            auto& ph = phases_[c];
            for (auto& v : buffer_) {
                v += cfloat(static_cast<float>(ph.real() * amp), static_cast<float>(ph.imag() * amp));
                ph *= step;
            }
            ph /= std::abs(ph);  // keep the rotator on the unit circle
        }

        if (options_.loopback) mix_tx(s, gain_amp);

        // AGC: steer the peak towards -12 dBFS (fast_attack reacts quicker)
        float peak = 0;
        for (const auto& v : buffer_) peak = std::max(peak, std::max(std::abs(v.real()), std::abs(v.imag())));
        if (s.gain_mode != "manual" && peak > 0) {
            const double err = -12.0 - 20 * std::log10(peak);
            const double step = s.gain_mode == "fast_attack" ? 0.5 : 0.1;
            std::lock_guard lock(mutex_);
            state_.gain = std::clamp(state_.gain + err * step, Limits::rx_gain_min_db, Limits::rx_gain_max_db);
        }

        for (std::size_t i = 0; i < n; ++i) {
            iq[2 * i] = static_cast<std::int16_t>(std::clamp(buffer_[i].real() * 32767.0f, -32768.0f, 32767.0f));
            iq[2 * i + 1] = static_cast<std::int16_t>(std::clamp(buffer_[i].imag() * 32767.0f, -32768.0f, 32767.0f));
        }
    }

    void mix_tx(const State& s, double gain_amp) {
        std::lock_guard lock(tx_mutex_);
        if (!tx_active_) return;
        const double offset = s.tx_lo - s.rx_lo;
        if (std::abs(offset) >= std::min(s.rate, s.rx_bw) / 2.0) return;
        const double amp = db_to_amp(kLoopbackCouplingDb - s.tx_atten) * gain_amp;
        const double w = 2 * M_PI * offset / s.rate;
        const std::complex<double> step(std::cos(w), std::sin(w));
        for (auto& v : buffer_) {
            cfloat x;
            if (tx_cyclic_) {
                if (tx_pattern_.empty()) return;
                x = tx_pattern_[tx_pos_];
                tx_pos_ = (tx_pos_ + 1) % tx_pattern_.size();
            } else {
                if (tx_head_ >= tx_ring_.size()) break;
                x = tx_ring_[tx_head_++];
            }
            const auto shifted = std::complex<double>(x.real(), x.imag()) * tx_phase_ * amp;
            v += cfloat(static_cast<float>(shifted.real()), static_cast<float>(shifted.imag()));
            tx_phase_ *= step;
        }
        tx_phase_ /= std::abs(tx_phase_);
    }

    SimOptions options_;
    std::mt19937 rng_;
    std::vector<cfloat> noise_;
    std::vector<std::complex<double>> phases_;
    std::vector<cfloat> buffer_;

    std::mutex mutex_;
    State state_;

    std::size_t rx_samples_ = 4096;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> rx_overflow_{false};
    Clock::time_point deadline_;

    std::mutex tx_mutex_;
    bool tx_active_ = false;
    bool tx_cyclic_ = false;
    std::size_t tx_samples_ = 0;
    std::vector<cfloat> tx_ring_;   // queued TX samples from tx_head_ on
    std::size_t tx_head_ = 0;
    std::vector<cfloat> tx_pattern_;
    std::size_t tx_pos_ = 0;
    std::uint64_t tx_writes_ = 0;
    Clock::time_point tx_deadline_;
    std::complex<double> tx_phase_{1.0, 0.0};
    std::atomic<bool> tx_underflow_{false};
};

}  // namespace

std::unique_ptr<Backend> make_sim_backend(SimOptions options) { return std::make_unique<SimBackend>(options); }

}  // namespace radio
