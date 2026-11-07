#include "channel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace channel {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kQuiet = 1e-12;        // |x|^2 below this is "no signal" (cs16 zero)
constexpr double kAssumedPower = 0.063; // -12 dBFS until a signal has been measured

// clock offset: fractional delay
constexpr int kTaps = 8;                // windowed sinc taps per output sample
constexpr int kPhases = 256;            // fractional positions (1/256 sample)
constexpr int kBaseDelay = 40;          // fixed delay so the extra delay can go +-kMaxTau
constexpr double kMaxTau = 32;
constexpr std::size_t kClockHist = kBaseDelay + 32 + kTaps + 2;

// multipath: windowed sinc for fractional path delays
constexpr int kSincHalf = 8;

// fading: sinusoids, coefficient updated every kFadeEvery samples (linear in between)
constexpr int kFadeSinusoids = 16;
constexpr std::size_t kFadeEvery = 16;

// oscillators: exact phase every kOscBlock samples, a rotator in between
constexpr std::size_t kOscBlock = 64;

double db_to_amp(double db) { return std::pow(10.0, db / 20); }
double db_to_pow(double db) { return std::pow(10.0, db / 10); }

double blackman_sinc(double t, double half) {
    if (std::abs(t) >= half) return 0;
    const double s = std::abs(t) < 1e-12 ? 1.0 : std::sin(kPi * t) / (kPi * t);
    const double w = 0.42 + 0.5 * std::cos(kPi * t / half) + 0.08 * std::cos(2 * kPi * t / half);
    return s * w;
}

// Polyphase table for the clock stage: taps for a position i0 + p / kPhases.
const std::array<std::array<float, kTaps>, kPhases + 1>& clock_table() {
    static const auto table = [] {
        std::array<std::array<float, kTaps>, kPhases + 1> t{};
        for (int p = 0; p <= kPhases; ++p) {
            const double f = static_cast<double>(p) / kPhases;
            double sum = 0;
            std::array<double, kTaps> h{};
            for (int k = 0; k < kTaps; ++k) {
                h[k] = blackman_sinc(k - (kTaps / 2 - 1) - f, kTaps / 2 + 1);
                sum += h[k];
            }
            for (int k = 0; k < kTaps; ++k) t[p][k] = static_cast<float>(h[k] / sum);
        }
        return t;
    }();
    return table;
}

// Inverse normal CDF (Acklam), for the Gaussian table.
double inverse_normal(double p) {
    static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                               1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                               6.680131188771972e+01, -1.328068155288572e+01};
    static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                               -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
    static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                               3.754408661907416e+00};
    const double lo = 0.02425, hi = 1 - lo;
    if (p < lo) {
        const double q = std::sqrt(-2 * std::log(p));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    }
    if (p > hi) {
        const double q = std::sqrt(-2 * std::log(1 - p));
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    }
    const double q = p - 0.5, r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
}

// 65536 equally likely Gaussian values (unit variance): a lookup per sample.
const std::vector<float>& gauss_table() {
    static const auto t = [] {
        std::vector<float> v(65536);
        double sq = 0;
        for (std::size_t i = 0; i < v.size(); ++i) {
            const double x = inverse_normal((static_cast<double>(i) + 0.5) / static_cast<double>(v.size()));
            v[i] = static_cast<float>(x);
            sq += x * x;
        }
        const auto g = static_cast<float>(1.0 / std::sqrt(sq / static_cast<double>(v.size())));  // exactly unit variance
        for (auto& x : v) x *= g;
        return v;
    }();
    return t;
}

cf rotate(cf v, float c, float s) { return {v.real() * c - v.imag() * s, v.real() * s + v.imag() * c}; }

}  // namespace

Channel::Channel(double sample_rate, std::uint64_t seed) : rate_(sample_rate), rng_(seed * 0x9E3779B97F4A7C15ull + 1) {
    gauss_table();
    clock_table();
    rebuild();
}

void Channel::set_sample_rate(double hz) {
    if (hz > 0 && hz != rate_) {
        rate_ = hz;
        rebuild();
    }
}

void Channel::configure(const Settings& s) {
    const bool clock_was = s_.clock;
    s_ = s;
    if (!s_.clock || !clock_was) {
        clock_hist_.assign(kClockHist, cf{});
        tau_ = 0;
    }
    rebuild();
}

std::uint32_t Channel::next_u32() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    return static_cast<std::uint32_t>(rng_ >> 32);
}

float Channel::gauss() { return gauss_table()[next_u32() >> 16]; }

void Channel::rebuild() {
    if (clock_hist_.size() != kClockHist) clock_hist_.assign(kClockHist, cf{});

    // multipath FIR: every path a windowed sinc at its delay, total power 1
    mp_taps_.clear();
    if (s_.multipath && !s_.paths.empty()) {
        double max_delay = 0, power = 0;
        for (const auto& p : s_.paths) {
            max_delay = std::max(max_delay, p.delay_us * 1e-6 * rate_);
            power += db_to_pow(p.gain_db);
        }
        const auto len = static_cast<std::size_t>(std::ceil(max_delay)) + 2 * kSincHalf + 1;
        std::vector<std::complex<double>> h(len);
        for (const auto& p : s_.paths) {
            const double d = p.delay_us * 1e-6 * rate_;
            const auto a = std::polar(db_to_amp(p.gain_db) / std::sqrt(power), p.phase_deg * kPi / 180);
            for (std::size_t n = 0; n < len; ++n) h[n] += a * blackman_sinc(static_cast<double>(n) - kSincHalf - d, kSincHalf);
        }
        for (const auto& v : h) mp_taps_.push_back(cf(v));
    }
    if (mp_hist_.size() + 1 != std::max<std::size_t>(mp_taps_.size(), 1)) {
        mp_hist_.assign(mp_taps_.empty() ? 0 : mp_taps_.size() - 1, cf{});
    }

    // fading: Jakes-like sum of sinusoids; keep the phasors when only K changes
    if (s_.fading && (s_.fading_hz != fading_hz_built_ || rate_ != fading_rate_built_)) {
        const double theta = 2 * kPi * (next_u32() / 4294967296.0);
        const bool fresh = fade_phasor_.size() != kFadeSinusoids;
        if (fresh) fade_phasor_.resize(kFadeSinusoids);
        fade_step_.resize(kFadeSinusoids);
        for (int n = 0; n < kFadeSinusoids; ++n) {
            const double alpha = (2 * kPi * n + theta) / kFadeSinusoids;
            const double w = 2 * kPi * s_.fading_hz * std::cos(alpha) * kFadeEvery / rate_;
            fade_step_[n] = cf(std::polar(1.0, w));
            if (fresh) fade_phasor_[n] = cf(std::polar(1.0, 2 * kPi * (next_u32() / 4294967296.0)));
        }
        fade_los_step_ = cf(std::polar(1.0, 2 * kPi * s_.fading_hz * std::cos(kPi / 4) * kFadeEvery / rate_));
        fading_hz_built_ = s_.fading_hz;
        fading_rate_built_ = rate_;
        fade_pos_ = 0;
    }
    const double k = db_to_pow(s_.rician_k_db);
    fade_los_amp_ = static_cast<float>(std::sqrt(k / (k + 1)));
    fade_scatter_amp_ = static_cast<float>(std::sqrt(1 / (k + 1)) / std::sqrt(static_cast<double>(kFadeSinusoids)));
}

void Channel::process(std::int16_t* iq, std::size_t n) {
    if (s_.bypass) {
        stats_.samples += n;
        return;
    }
    std::vector<cf> x(n);
    constexpr float in = 1.0f / 32768.0f;
    for (std::size_t i = 0; i < n; ++i) x[i] = cf(iq[2 * i] * in, iq[2 * i + 1] * in);
    process(x.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
        const float re = x[i].real() * 32768.0f, im = x[i].imag() * 32768.0f;
        if (re > 32767.0f || re < -32768.0f || im > 32767.0f || im < -32768.0f) ++stats_.clipped;
        iq[2 * i] = static_cast<std::int16_t>(std::lrint(std::clamp(re, -32768.0f, 32767.0f)));
        iq[2 * i + 1] = static_cast<std::int16_t>(std::lrint(std::clamp(im, -32768.0f, 32767.0f)));
    }
}

void Channel::process(cf* x, std::size_t n) {
    stats_.samples += n;
    if (s_.bypass || n == 0) return;

    // Signal power while present (zeros = gaps between packets do not count).
    double sum = 0;
    std::size_t present = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double p = std::norm(x[i]);
        if (p > kQuiet) {
            sum += p;
            ++present;
        }
    }
    if (present > n / 16) {
        const double p = sum / static_cast<double>(present);
        p_signal_ = p_signal_ < 0 ? p : 0.8 * p_signal_ + 0.2 * p;
    }
    const double p_in = p_signal_ < 0 ? kAssumedPower : p_signal_;
    stats_.signal_dbfs = 10 * std::log10(std::max(p_in, 1e-20));

    if (s_.clock) stage_clock(x, n);
    if (s_.multipath && !mp_taps_.empty()) stage_multipath(x, n);
    if (s_.fading) stage_fading(x, n);
    const auto gain = static_cast<float>(db_to_amp(s_.gain_db));
    if (gain != 1.0f) {
        for (std::size_t i = 0; i < n; ++i) x[i] *= gain;
    }
    if (s_.frequency || s_.phase_noise) stage_frequency(x, n);

    // Power of the signal as it arrives: what DC, interference and noise refer to.
    const double p_out = p_in * gain * gain;

    if (s_.iq) {
        const double g = db_to_amp(s_.iq_gain_db), phi = s_.iq_phase_deg * kPi / 180;
        const cf mu(std::complex<double>(0.5, 0) * (1.0 + g * std::polar(1.0, -phi)));
        const cf nu(std::complex<double>(0.5, 0) * (1.0 - g * std::polar(1.0, phi)));
        for (std::size_t i = 0; i < n; ++i) x[i] = mu * x[i] + nu * std::conj(x[i]);
    }
    if (s_.dc) {
        const cf dc(std::polar(std::sqrt(p_out * db_to_pow(s_.dc_dbc)), s_.dc_phase_deg * kPi / 180));
        for (std::size_t i = 0; i < n; ++i) x[i] += dc;
    }
    if (s_.interference) {
        const auto amp = static_cast<float>(std::sqrt(p_out * db_to_pow(s_.interference_dbc)));
        const double w = 2 * kPi * s_.interference_hz / rate_;
        const double period = (s_.interference_on_ms + s_.interference_off_ms) * 1e-3;
        const bool pulsed = s_.interference_on_ms > 0 && s_.interference_off_ms > 0;
        for (std::size_t i = 0; i < n; i += kOscBlock) {
            const std::size_t m = std::min(kOscBlock, n - i);
            const bool on = !pulsed || std::fmod(tone_t_, period) < s_.interference_on_ms * 1e-3;
            if (on) {
                cf r(std::polar(1.0, tone_phase_));
                const cf step(std::polar(1.0, w));
                for (std::size_t k = 0; k < m; ++k) {
                    x[i + k] += amp * r;
                    r *= step;
                }
            }
            tone_phase_ = std::fmod(tone_phase_ + w * static_cast<double>(m), 2 * kPi);
            tone_t_ += static_cast<double>(m) / rate_;
        }
        if (pulsed) tone_t_ = std::fmod(tone_t_, period);
    }
    if (s_.noise) {
        double p_noise = p_out * db_to_pow(-s_.snr_db);
        if (s_.noise_bandwidth_hz > 0) p_noise *= rate_ / s_.noise_bandwidth_hz;  // SNR in that bandwidth
        const auto sigma = static_cast<float>(std::sqrt(p_noise / 2));
        for (std::size_t i = 0; i < n; ++i) x[i] += cf(sigma * gauss(), sigma * gauss());
    }
}

// Transmitter sample clock off by `clock_ppm`: the signal is read at
// k * (1 + e), i.e. a delay that drifts by e per sample (fractional delay
// filter). The delay goes back to zero in a quiet gap (between packets, where
// the jump is invisible); a signal without gaps has it forced back at +-32
// samples (counted). The number of samples is kept: the stream's pacing
// (TX paced by RX) is not disturbed.
void Channel::stage_clock(cf* x, std::size_t n) {
    const auto& table = clock_table();
    const double e = s_.clock_ppm * 1e-6;
    const std::size_t h = clock_hist_.size();
    std::vector<cf> buf(h + n);
    std::copy(clock_hist_.begin(), clock_hist_.end(), buf.begin());
    std::copy(x, x + n, buf.begin() + static_cast<std::ptrdiff_t>(h));
    for (std::size_t k = 0; k < n; ++k) {
        if (std::norm(buf[h + k]) <= kQuiet) {
            ++quiet_run_;
        } else {
            quiet_run_ = 0;
        }
        if (quiet_run_ >= kClockHist) {  // everything the filter sees is zero: start over unseen
            if (tau_ != 0) ++stats_.timing_resets;
            tau_ = 0;
        } else {
            tau_ -= e;  // a faster transmitter: its samples arrive a little earlier each time
        }
        if (std::abs(tau_) > kMaxTau) {
            tau_ = 0;
            ++stats_.timing_forced;
        }
        // read the input at (h + k) - kBaseDelay - tau
        const double pos = static_cast<double>(h + k) - kBaseDelay - tau_;
        const auto i0 = static_cast<std::ptrdiff_t>(std::floor(pos));
        const auto p = static_cast<int>(std::lround((pos - static_cast<double>(i0)) * kPhases));
        const auto& t = table[p];
        const cf* in = buf.data() + i0 - (kTaps / 2 - 1);
        float re = 0, im = 0;
        for (int j = 0; j < kTaps; ++j) {
            re += t[j] * in[j].real();
            im += t[j] * in[j].imag();
        }
        x[k] = cf(re, im);
    }
    std::copy(buf.end() - static_cast<std::ptrdiff_t>(h), buf.end(), clock_hist_.begin());
}

void Channel::stage_multipath(cf* x, std::size_t n) {
    const std::size_t L = mp_taps_.size(), h = mp_hist_.size();
    std::vector<cf> buf(h + n);
    std::copy(mp_hist_.begin(), mp_hist_.end(), buf.begin());
    std::copy(x, x + n, buf.begin() + static_cast<std::ptrdiff_t>(h));
    for (std::size_t k = 0; k < n; ++k) {
        const cf* in = buf.data() + h + k;  // newest sample; older at in[-j]
        float re = 0, im = 0;
        for (std::size_t j = 0; j < L; ++j) {
            const cf a = mp_taps_[j], b = *(in - static_cast<std::ptrdiff_t>(j));
            re += a.real() * b.real() - a.imag() * b.imag();
            im += a.real() * b.imag() + a.imag() * b.real();
        }
        x[k] = cf(re, im);
    }
    std::copy(buf.end() - static_cast<std::ptrdiff_t>(h), buf.end(), mp_hist_.begin());
}

void Channel::stage_fading(cf* x, std::size_t n) {
    for (std::size_t k = 0; k < n; ++k) {
        if (fade_pos_ == 0) {
            cf sum{};
            for (int s = 0; s < kFadeSinusoids; ++s) {
                fade_phasor_[s] *= fade_step_[s];
                sum += fade_phasor_[s];
            }
            fade_los_ *= fade_los_step_;
            if ((++fade_updates_ & 1023) == 0) {  // keep the phasors on the unit circle
                for (auto& p : fade_phasor_) p /= std::abs(p);
                fade_los_ /= std::abs(fade_los_);
            }
            const cf next = fade_scatter_amp_ * sum + fade_los_amp_ * fade_los_;
            fade_delta_ = (next - fade_cur_) / static_cast<float>(kFadeEvery);
        }
        x[k] *= fade_cur_;
        fade_cur_ += fade_delta_;
        if (++fade_pos_ == kFadeEvery) fade_pos_ = 0;
    }
}

// Frequency offset with sinusoidal Doppler, and Wiener phase noise. The phase
// is exact (double) at the start of every kOscBlock samples; in between a
// rotator turns it, the phase noise steps as small rotations.
void Channel::stage_frequency(cf* x, std::size_t n) {
    const float pn_sigma = s_.phase_noise ? static_cast<float>(std::sqrt(2 * kPi * s_.linewidth_hz / rate_)) : 0.0f;
    for (std::size_t i = 0; i < n; i += kOscBlock) {
        const std::size_t m = std::min(kOscBlock, n - i);
        double f = 0;
        if (s_.frequency) {
            f = s_.cfo_hz;
            if (s_.doppler_hz != 0 && s_.doppler_period_s > 0) {
                f += s_.doppler_hz * std::sin(2 * kPi * doppler_t_ / s_.doppler_period_s);
                doppler_t_ = std::fmod(doppler_t_ + static_cast<double>(m) / rate_, s_.doppler_period_s);
            }
        }
        const double w = 2 * kPi * f / rate_;
        const auto start = std::polar(1.0, freq_phase_ + pn_phase_);
        float rr = static_cast<float>(start.real()), ri = static_cast<float>(start.imag());
        const auto c = static_cast<float>(std::cos(w)), s = static_cast<float>(std::sin(w));
        double pn_walk = 0;
        for (std::size_t k = 0; k < m; ++k) {
            x[i + k] = rotate(x[i + k], rr, ri);
            float nr = rr * c - ri * s, ni = rr * s + ri * c;
            if (pn_sigma > 0) {
                const float d = pn_sigma * gauss();
                pn_walk += d;
                const float dc = 1 - 0.5f * d * d;  // cos d, sin d for a small d
                const float t = nr * dc - ni * d;
                ni = nr * d + ni * dc;
                nr = t;
            }
            rr = nr;
            ri = ni;
        }
        freq_phase_ = std::fmod(freq_phase_ + w * static_cast<double>(m), 2 * kPi);
        pn_phase_ = std::fmod(pn_phase_ + pn_walk, 2 * kPi);
    }
}

// ---------------------------------------------------------------------------

std::vector<std::string> preset_names() {
    return {"clean", "awgn-30", "awgn-25", "awgn-20", "awgn-15", "awgn-10", "offset-5k", "doppler", "oscillators",
            "indoor", "urban", "mobile", "interference", "bad-radio"};
}

Settings preset(const std::string& name) {
    Settings s;
    auto noise = [&s](double snr) {
        s.noise = true;
        s.snr_db = snr;
    };
    if (name == "clean") {
    } else if (name.rfind("awgn-", 0) == 0) {
        noise(std::stod(name.substr(5)));
    } else if (name == "offset-5k") {
        s.frequency = true;
        s.cfo_hz = 5000;
        noise(30);
    } else if (name == "doppler") {  // a LEO pass at 2 GHz: +-50 kHz over minutes, scaled down
        s.frequency = true;
        s.doppler_hz = 8000;
        s.doppler_period_s = 20;
        noise(30);
    } else if (name == "oscillators") {
        s.frequency = true;
        s.cfo_hz = 1500;
        s.phase_noise = true;
        s.linewidth_hz = 20;  // a good synthesizer is far below; the modem copes up to about 50 Hz
        s.clock = true;
        s.clock_ppm = 20;
        noise(30);
    } else if (name == "indoor") {
        s.multipath = true;
        s.paths = {{0, 0, 0}, {0.1, -6, 70}, {0.25, -12, 200}};
        noise(30);
    } else if (name == "urban") {
        s.multipath = true;
        s.paths = {{0, 0, 0}, {0.5, -3, 90}, {1.3, -8, 200}, {2.7, -14, 30}};
        noise(30);
    } else if (name == "mobile") {
        s.fading = true;
        s.fading_hz = 50;
        s.rician_k_db = 6;
        s.frequency = true;
        s.cfo_hz = 500;
        noise(25);
    } else if (name == "interference") {
        s.interference = true;
        s.interference_hz = 400e3;
        s.interference_dbc = -10;
        s.interference_on_ms = 2;
        s.interference_off_ms = 8;
        noise(30);
    } else if (name == "bad-radio") {
        s.iq = true;
        s.iq_gain_db = 1;
        s.iq_phase_deg = 3;
        s.dc = true;
        s.dc_dbc = -25;
        noise(30);
    } else {
        throw std::invalid_argument("unknown channel preset '" + name + "'");
    }
    return s;
}

}  // namespace channel
