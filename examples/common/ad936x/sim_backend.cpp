// Simulated AD9364 (or AD9361 with two RX channels) for development and tests
// without hardware.
//
// The "air" contains a few carriers at fixed RF frequencies. Whatever falls
// inside the tuned band (LO +- min(rate, bandwidth)/2) shows up at its offset
// from the LO, scaled by the RX gain, on top of a noise floor that also follows
// the gain. Samples are paced in real time like the real device: a reader that
// falls behind by more than a buffer gets an overflow. With loopback enabled,
// what TX channel `loopback_tx` sends is mixed into RX channel `loopback_rx`
// (like a TX1 -> RX2 cable), shifted by (TX LO - RX LO). RX1 and RX2 share LO
// and rate, have their own gain / AGC and independent noise.

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

double db_to_amp(double db) { return std::pow(10.0, db / 20.0); }

class SimBackend final : public Backend {
public:
    explicit SimBackend(SimOptions o) : options_(o), rng_(o.seed) {
        std::normal_distribution<float> n(0.0f, std::sqrt(0.5f));
        noise_.resize(1 << 16);
        for (auto& v : noise_) v = {n(rng_), n(rng_)};
        options_.channels = std::clamp(options_.channels, 1u, 2u);
        options_.loopback_rx = std::min(options_.loopback_rx, options_.channels - 1);
        options_.loopback_tx = std::min(options_.loopback_tx, options_.channels - 1);
        for (auto& p : phases_) p.assign(carriers().size(), {1.0, 0.0});
    }

    std::string name() const override { return "sim"; }

    nlohmann::json describe() const override {
        return {{"backend", "sim"},
                {"model", options_.channels > 1 ? "Simulated AD9361" : "Simulated AD9364"},
                {"rx_channels", options_.channels},
                {"tx_channels", options_.channels},
                {"loopback", options_.loopback
                                 ? nlohmann::json(fmt_loopback())
                                 : nlohmann::json(false)},
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
    void set_gain_mode(const std::string& mode, unsigned ch) override {
        check_channel(ch);
        std::lock_guard lock(mutex_);
        state_.gain_mode[ch] = mode;
    }
    void set_rx_gain(double db, unsigned ch) override {
        check_channel(ch);
        std::lock_guard lock(mutex_);
        if (state_.gain_mode[ch] != "manual") throw DeviceError("RX gain can only be set in manual gain mode");
        state_.gain[ch] = db;
    }
    void set_tx_attenuation(double db, unsigned ch) override {
        check_channel(ch, "TX");
        std::lock_guard lock(mutex_);
        state_.tx_atten[ch] = std::round(db * 4) / 4;
    }
    double rx_gain(unsigned ch) override {
        check_channel(ch);
        std::lock_guard lock(mutex_);
        return state_.gain[ch];
    }
    unsigned rx_channel_count() const override { return options_.channels; }
    unsigned tx_channel_count() const override { return options_.channels; }
    std::optional<double> temperature_c() override {
        const auto t = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
        return 41.5 + 1.5 * std::sin(t / 60.0);
    }

    // --- RX ------------------------------------------------------------------------
    void start_rx(std::size_t samples, unsigned mask) override {
        if ((mask & kRx2) && options_.channels < 2) check_channel(1, "RX");
        if ((mask & (kRx1 | kRx2)) == 0) throw DeviceError("no RX channel selected");
        rx_mask_ = mask;
        rx_samples_ = std::max<std::size_t>(samples, 64);
        cancelled_ = false;
        deadline_ = Clock::now();
    }

    std::size_t read_rx(std::int16_t* rx1, std::int16_t* rx2, std::size_t max_samples) override {
        if (cancelled_) return 0;
        const State s = snapshot();
        const std::size_t n = std::min(max_samples, rx_samples_);
        const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(n / s.rate));

        // Real-time pacing; a consumer that is late by a whole buffer lost samples.
        deadline_ += period;
        const auto now = Clock::now();
        if (now > deadline_ + period) {
            rx_overflow_ = true;
            // Nobody read for a while, but the DAC kept playing: drain TX by as much.
            const double lost = std::chrono::duration<double>(now - deadline_).count();
            {
                std::lock_guard lock(tx_mutex_);
                if (tx_active_ && !tx_cyclic_) {
                    tx_head_ = std::min(tx_ring_.size(), tx_head_ + static_cast<std::size_t>(lost * s.rate));
                }
            }
            deadline_ = now;
        }
        while (Clock::now() < deadline_) {
            if (cancelled_) return 0;
            std::this_thread::sleep_for(std::min<Clock::duration>(deadline_ - Clock::now(), std::chrono::milliseconds(20)));
        }

        // The TX queue drains like the hardware's, whether or not a channel is read;
        // what leaves TX is taken once and reaches every RX it is cabled to.
        take_tx(s, n);
        for (unsigned ch = 0; ch < 2; ++ch) {
            if (!(rx_mask_ & (1u << ch))) continue;
            std::int16_t* out = ch == 0 ? rx1 : rx2;
            if (out) generate(s, ch, out, n);
        }
        return n;
    }

    void stop_rx() override {}
    void cancel_rx() override { cancelled_ = true; }
    bool take_rx_overflow() override { return rx_overflow_.exchange(false); }

    // --- TX ------------------------------------------------------------------------
    void start_tx(std::size_t samples, bool cyclic, unsigned ch) override {
        check_channel(ch, "TX");
        std::lock_guard lock(tx_mutex_);
        tx_channel_ = ch;
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
        double gain[2] = {30, 30};
        double tx_atten[2] = {10, 10};
        std::string gain_mode[2] = {"slow_attack", "slow_attack"};
    };

    void check_channel(unsigned ch, const char* dir = "RX") const {
        if (ch >= options_.channels) {
            throw DeviceError(std::string("this transceiver has one ") + dir + " channel (AD9364); " + dir +
                              "2 needs an AD9361/AD9363");
        }
    }

    std::string fmt_loopback() const {
        const std::string tx = "TX" + std::to_string(options_.loopback_tx + 1);
        return options_.loopback_split ? tx + " -> splitter -> every RX" : tx + " -> RX" + std::to_string(options_.loopback_rx + 1);
    }

    void set(double State::*field, double value) {
        std::lock_guard lock(mutex_);
        state_.*field = value;
    }

    State snapshot() {
        std::lock_guard lock(mutex_);
        return state_;
    }

    // A carrier inside the band: a complex rotator advanced one sample at a time.
    // Plain float arithmetic: std::complex's operator* goes through __mulsc3
    // (NaN/inf handling) and costs several times as much.
    struct Rotator {
        float re = 1, im = 0;          // current phase
        float step_re = 1, step_im = 0;
        float amp = 0;
        void renormalize() {
            const float m = std::sqrt(re * re + im * im);
            if (m > 0) {
                re /= m;
                im /= m;
            }
        }
    };

    // The receive signal of one channel: noise floor + carriers + TX loopback,
    // with the AGC. Done in blocks that stay in L1: one pass per block instead
    // of a pass over the whole buffer per step.
    void generate(const State& s, unsigned ch, std::int16_t* iq, std::size_t n) {
        constexpr std::size_t kBlock = 1024;
        buffer_.resize(kBlock);
        const double half_band = std::min(s.rate, s.rx_bw) / 2.0;
        const double gain_amp = db_to_amp(s.gain[ch]);
        const float noise_amp = static_cast<float>(db_to_amp(kNoiseDbfsAt0dB) * gain_amp);
        std::size_t idx = std::uniform_int_distribution<std::size_t>(0, noise_.size() - 1)(rng_);

        rotators_.clear();
        std::vector<std::size_t> which;
        for (std::size_t c = 0; c < carriers().size(); ++c) {
            const double offset = carriers()[c].hz - s.rx_lo;
            if (std::abs(offset) >= half_band) continue;
            const double w = 2 * M_PI * offset / s.rate;
            const auto& ph = phases_[ch][c];
            Rotator r;
            r.re = static_cast<float>(ph.real());
            r.im = static_cast<float>(ph.imag());
            r.step_re = static_cast<float>(std::cos(w));
            r.step_im = static_cast<float>(std::sin(w));
            r.amp = static_cast<float>(db_to_amp(carriers()[c].dbfs_at_0db) * gain_amp);
            rotators_.push_back(r);
            which.push_back(c);
        }
        const bool loopback = tx_now_active_ && options_.loopback &&
                              (options_.loopback_split ? ch < options_.channels : ch == options_.loopback_rx);

        float peak = 0;
        for (std::size_t done = 0; done < n; done += kBlock) {
            const std::size_t m = std::min(kBlock, n - done);
            cfloat* v = buffer_.data();
            // noise (from a precomputed table, random start per buffer), in contiguous runs
            for (std::size_t i = 0; i < m;) {
                const std::size_t run = std::min(m - i, noise_.size() - idx);
                const cfloat* src = noise_.data() + idx;
                for (std::size_t k = 0; k < run; ++k) v[i + k] = src[k] * noise_amp;
                i += run;
                idx = (idx + run) & (noise_.size() - 1);
            }
            for (auto& r : rotators_) add_carrier(r, v, m);
            if (loopback) add_tx(s, gain_amp, v, done, m);
            // peak (for the AGC) and int16 conversion in the same pass
            std::int16_t* out = iq + 2 * done;
            for (std::size_t i = 0; i < m; ++i) {
                const float re = v[i].real(), im = v[i].imag();
                peak = std::max(peak, std::max(std::fabs(re), std::fabs(im)));
                out[2 * i] = static_cast<std::int16_t>(std::clamp(re * 32767.0f, -32768.0f, 32767.0f));
                out[2 * i + 1] = static_cast<std::int16_t>(std::clamp(im * 32767.0f, -32768.0f, 32767.0f));
            }
        }
        for (std::size_t k = 0; k < which.size(); ++k) phases_[ch][which[k]] = {rotators_[k].re, rotators_[k].im};

        // AGC: steer the peak towards -12 dBFS (fast_attack reacts quicker)
        if (s.gain_mode[ch] != "manual" && peak > 0) {
            const double err = -12.0 - 20 * std::log10(peak);
            const double step = s.gain_mode[ch] == "fast_attack" ? 0.5 : 0.1;
            std::lock_guard lock(mutex_);
            state_.gain[ch] = std::clamp(state_.gain[ch] + err * step, Limits::rx_gain_min_db, Limits::rx_gain_max_db);
        }
    }

    // v[i] += amp * phase * step^i. Eight independent lanes (each advanced by
    // step^8) instead of one chain: no sample waits for the previous one, so the
    // compiler can vectorize it.
    static void add_carrier(Rotator& r, cfloat* v, std::size_t m) {
        constexpr std::size_t L = 8;
        float re[L], im[L];
        re[0] = r.re;
        im[0] = r.im;
        for (std::size_t j = 1; j < L; ++j) {
            re[j] = re[j - 1] * r.step_re - im[j - 1] * r.step_im;
            im[j] = re[j - 1] * r.step_im + im[j - 1] * r.step_re;
        }
        // step^8
        float s8re = r.step_re, s8im = r.step_im;
        for (int k = 0; k < 3; ++k) {
            const float t = s8re * s8re - s8im * s8im;
            s8im = 2 * s8re * s8im;
            s8re = t;
        }
        float* f = reinterpret_cast<float*>(v);  // std::complex<float> is {re, im}
        std::size_t i = 0;
        for (; i + L <= m; i += L) {
            for (std::size_t j = 0; j < L; ++j) {
                f[2 * (i + j)] += re[j] * r.amp;
                f[2 * (i + j) + 1] += im[j] * r.amp;
                const float nre = re[j] * s8re - im[j] * s8im;
                im[j] = re[j] * s8im + im[j] * s8re;
                re[j] = nre;
            }
        }
        // the rest one by one; lane 0 is the phase of sample i
        float pre = re[0], pim = im[0];
        for (; i < m; ++i) {
            f[2 * i] += pre * r.amp;
            f[2 * i + 1] += pim * r.amp;
            const float nre = pre * r.step_re - pim * r.step_im;
            pim = pre * r.step_im + pim * r.step_re;
            pre = nre;
        }
        r.re = pre;
        r.im = pim;
        r.renormalize();  // keep it on the unit circle (float rounding)
    }

    // TX -> RX cable of the simulated setup, shifted by TX LO - RX LO.
    // What TX sends during the next `n` samples, shifted by TX LO - RX LO (+ the
    // simulated oscillator offset). Empty when TX is off or outside the RX band.
    void take_tx(const State& s, std::size_t n) {
        std::lock_guard lock(tx_mutex_);
        tx_now_active_ = false;
        if (!options_.loopback || !tx_active_ || tx_channel_ != options_.loopback_tx) return;
        const double offset = s.tx_lo - s.rx_lo + options_.loopback_cfo_hz;
        if (std::abs(offset) >= std::min(s.rate, s.rx_bw) / 2.0) {
            if (!tx_cyclic_) tx_head_ = std::min(tx_ring_.size(), tx_head_ + n);  // drains anyway
            return;
        }
        tx_now_.assign(n, {});
        tx_now_atten_db_ = s.tx_atten[tx_channel_];
        const double w = 2 * M_PI * offset / s.rate;
        const float step_re = static_cast<float>(std::cos(w)), step_im = static_cast<float>(std::sin(w));
        float pre = static_cast<float>(tx_phase_.real()), pim = static_cast<float>(tx_phase_.imag());
        for (std::size_t i = 0; i < n; ++i) {
            cfloat x;
            if (tx_cyclic_) {
                if (tx_pattern_.empty()) break;
                x = tx_pattern_[tx_pos_];
                if (++tx_pos_ == tx_pattern_.size()) tx_pos_ = 0;
            } else {
                if (tx_head_ >= tx_ring_.size()) break;  // nothing queued: silence (an underflow)
                x = tx_ring_[tx_head_++];
            }
            tx_now_[i] = cfloat(x.real() * pre - x.imag() * pim, x.real() * pim + x.imag() * pre);
            const float nre = pre * step_re - pim * step_im;
            pim = pre * step_im + pim * step_re;
            pre = nre;
        }
        const float mag = std::sqrt(pre * pre + pim * pim);
        tx_phase_ = mag > 0 ? std::complex<double>(pre / mag, pim / mag) : std::complex<double>(1.0, 0.0);
        tx_now_active_ = true;
    }

    // TX -> RX cable(s) of the simulated setup: samples [from, from + m) of take_tx().
    void add_tx(const State&, double gain_amp, cfloat* v, std::size_t from, std::size_t m) {
        const float amp = static_cast<float>(db_to_amp(options_.loopback_db - tx_now_atten_db_) * gain_amp);
        const cfloat* x = tx_now_.data() + from;
        for (std::size_t i = 0; i < m; ++i) v[i] = cfloat(v[i].real() + x[i].real() * amp, v[i].imag() + x[i].imag() * amp);
    }

    SimOptions options_;
    std::mt19937 rng_;
    std::vector<cfloat> noise_;
    std::vector<std::complex<double>> phases_[2];
    std::vector<cfloat> buffer_;
    std::vector<Rotator> rotators_;
    std::vector<std::int16_t> scratch_;
    unsigned rx_mask_ = kRx1;

    std::mutex mutex_;
    State state_;

    std::size_t rx_samples_ = 4096;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> rx_overflow_{false};
    Clock::time_point deadline_;

    std::mutex tx_mutex_;
    bool tx_active_ = false;
    bool tx_cyclic_ = false;
    unsigned tx_channel_ = 0;
    std::size_t tx_samples_ = 0;
    std::vector<cfloat> tx_ring_;   // queued TX samples from tx_head_ on
    std::vector<cfloat> tx_now_;    // what TX sends during the current RX buffer (RX thread)
    bool tx_now_active_ = false;
    double tx_now_atten_db_ = 0;
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
