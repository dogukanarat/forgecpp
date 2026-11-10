#include "qam.hpp"

#include <algorithm>
#include <cmath>

namespace qam {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kConstellationPoints = 512;

// Continuous RRC impulse response; t in symbols.
double rrc(double t, double a) {
    if (std::abs(t) < 1e-9) return 1.0 - a + 4 * a / kPi;
    if (a > 0 && std::abs(std::abs(t) - 1.0 / (4 * a)) < 1e-9) {
        return a / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * a)) + (1 - 2 / kPi) * std::cos(kPi / (4 * a)));
    }
    const double num = std::sin(kPi * t * (1 - a)) + 4 * a * t * std::cos(kPi * t * (1 + a));
    const double den = kPi * t * (1 - (4 * a * t) * (4 * a * t));
    return num / den;
}

double rrc_norm(const Params& p) {
    const unsigned n = p.span * p.sps + 1, d = n / 2;
    double e = 0;
    for (unsigned i = 0; i < n; ++i) {
        const double v = rrc((static_cast<double>(i) - d) / p.sps, p.rolloff);
        e += v * v;
    }
    return 1.0 / std::sqrt(e);
}

const float kQ = static_cast<float>(1.0 / std::sqrt(2.0));
const float kA = static_cast<float>(1.0 / std::sqrt(10.0));

// 2-bit Gray: 00 -> -3, 01 -> -1, 11 -> +1, 10 -> +3
float gray_level(unsigned b) {
    static const float level[4] = {-3, -1, 3, 1};
    return level[b & 3];
}
unsigned gray_bits(float v) {  // v in units of kA
    if (v < -2) return 0;
    if (v < 0) return 1;
    if (v < 2) return 3;
    return 2;
}

cf qpsk(unsigned bits) { return {bits & 2 ? -kQ : kQ, bits & 1 ? -kQ : kQ}; }
unsigned qpsk_bits(cf v) { return (v.real() < 0 ? 2u : 0u) | (v.imag() < 0 ? 1u : 0u); }

}  // namespace

// ---------------------------------------------------------------------------
// building blocks

std::uint16_t crc16(const std::uint8_t* data, std::size_t n) {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i] << 8);
        for (int b = 0; b < 8; ++b)
            crc =
                (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021) : static_cast<std::uint16_t>(crc << 1);
    }
    return crc;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t n) {
    static const auto table = [] {
        std::vector<std::uint32_t> t(256);
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void scramble(std::uint8_t* data, std::size_t n) {
    // The sequence repeats every 127 bits, so every 127 bytes: a table.
    static const auto key = [] {
        std::vector<std::uint8_t> k(127);
        unsigned state = 0x5D;  // 7 bits
        for (auto& byte : k) {
            for (int b = 0; b < 8; ++b) {
                const unsigned bit = ((state >> 6) ^ (state >> 3)) & 1;
                state = ((state << 1) | bit) & 0x7F;
                byte = static_cast<std::uint8_t>((byte << 1) | bit);
            }
        }
        return k;
    }();
    for (std::size_t i = 0; i < n; ++i) data[i] ^= key[i % 127];
}

const std::vector<cf>& preamble() {
    static const std::vector<cf> p = [] {
        std::vector<cf> half;
        unsigned lfsr = 0x5A;  // x^7 + x^6 + 1
        for (unsigned i = 0; i < kPreambleHalf; ++i) {
            unsigned bits = 0;
            for (int b = 0; b < 2; ++b) {
                const unsigned bit = ((lfsr >> 6) ^ (lfsr >> 5)) & 1;
                lfsr = ((lfsr << 1) | bit) & 0x7F;
                bits = (bits << 1) | bit;
            }
            half.push_back(qpsk(bits));  // unit magnitude
        }
        std::vector<cf> all(half);
        all.insert(all.end(), half.begin(), half.end());
        return all;
    }();
    return p;
}

cf map16(unsigned nibble) { return {gray_level(nibble >> 2) * kA, gray_level(nibble) * kA}; }

unsigned demap16(cf s) { return (gray_bits(s.real() / kA) << 2) | gray_bits(s.imag() / kA); }

std::vector<float> rrc_taps(const Params& p) {
    const unsigned n = p.span * p.sps + 1, d = n / 2;
    const double norm = rrc_norm(p);
    std::vector<float> h(n);
    for (unsigned i = 0; i < n; ++i)
        h[i] = static_cast<float>(norm * rrc((static_cast<double>(i) - d) / p.sps, p.rolloff));
    return h;
}

std::size_t packet_symbol_count(std::size_t n) { return kPreambleSymbols + kHeaderSymbols + (n + 4) * 2 + kGapSymbols; }

std::vector<cf> packet_symbols(const std::uint8_t* payload, std::size_t n, std::uint16_t seq) {
    std::vector<cf> s(preamble());
    s.reserve(packet_symbol_count(n));
    std::uint8_t h[6] = {static_cast<std::uint8_t>(n >> 8),
                         static_cast<std::uint8_t>(n),
                         static_cast<std::uint8_t>(seq >> 8),
                         static_cast<std::uint8_t>(seq),
                         0,
                         0};
    const auto c16 = crc16(h, 4);
    h[4] = static_cast<std::uint8_t>(c16 >> 8);
    h[5] = static_cast<std::uint8_t>(c16);
    for (std::uint8_t byte : h) {
        for (int k = 3; k >= 0; --k) s.push_back(qpsk((byte >> (2 * k)) & 3));
    }
    std::vector<std::uint8_t> body(payload, payload + n);
    const auto c32 = crc32(body.data(), n);
    for (int k = 0; k < 4; ++k) body.push_back(static_cast<std::uint8_t>(c32 >> (8 * k)));
    scramble(body.data(), body.size());
    for (std::uint8_t byte : body) {
        s.push_back(map16(byte >> 4));
        s.push_back(map16(byte & 0xF));
    }
    return s;
}

// ---------------------------------------------------------------------------
// Modulator

Modulator::Modulator(const Params& p) : p_(p), taps_(rrc_taps(p)) {
    // Unit-energy symbols through unit-energy taps: sample power 1 / sps.
    scale_ = static_cast<float>(p.tx_rms * std::sqrt(static_cast<double>(p.sps)) * 32767.0);
    // Polyphase: output r of a symbol = sum_j poly_[r][j] * symbol[newest - j], scaled.
    len_ = p.span + 1;
    poly_.assign(std::size_t(p.sps) * len_, 0.0f);
    for (unsigned r = 0; r < p.sps; ++r) {
        for (unsigned j = 0; j < len_; ++j) {
            const std::size_t t = r + std::size_t(j) * p.sps;
            if (t < taps_.size()) poly_[std::size_t(r) * len_ + j] = taps_[t] * scale_;
        }
    }
    // The history twice in a row, so the newest len_ symbols are always contiguous.
    hist_re_.assign(2 * len_, 0.0f);
    hist_im_.assign(2 * len_, 0.0f);
}

void Modulator::next_symbol_samples(cf a) {
    const unsigned sps = p_.sps;
    const std::size_t at = ready_.size();
    ready_.resize(at + 2 * sps);
    std::int16_t* out = ready_.data() + at;
    // Idle (only zeros in the filter): zeros out, no arithmetic.
    quiet_ = (a == cf{}) ? quiet_ + 1 : 0;
    if (quiet_ > len_) {
        std::fill(out, out + 2 * sps, std::int16_t{0});
        return;
    }
    head_ = head_ == 0 ? len_ - 1 : head_ - 1;
    hist_re_[head_] = hist_re_[head_ + len_] = a.real();
    hist_im_[head_] = hist_im_[head_ + len_] = a.imag();
    const float* hr = hist_re_.data() + head_;
    const float* hi = hist_im_.data() + head_;
    for (unsigned r = 0; r < sps; ++r) {
        const float* h = poly_.data() + std::size_t(r) * len_;
        float re = 0, im = 0;
        for (unsigned j = 0; j < len_; ++j) {
            re += h[j] * hr[j];
            im += h[j] * hi[j];
        }
        if (std::abs(re) > 32767.0f || std::abs(im) > 32767.0f) ++clipped_;
        out[2 * r] = static_cast<std::int16_t>(std::clamp(re, -32767.0f, 32767.0f));
        out[2 * r + 1] = static_cast<std::int16_t>(std::clamp(im, -32767.0f, 32767.0f));
    }
}

void Modulator::fill(std::int16_t* iq, std::size_t n, const Next& next) {
    while ((ready_.size() - ready_pos_) / 2 < n) {
        if (ready_pos_ > 0 && ready_pos_ == ready_.size()) {
            ready_.clear();
            ready_pos_ = 0;
        }
        cf a{};
        if (pos_ < symbols_.size()) {
            a = symbols_[pos_++];
        } else if (gap_ > 0) {
            --gap_;
        } else {
            std::uint16_t seq = 0;
            payload_.clear();
            if (next && next(payload_, seq) && !payload_.empty() && payload_.size() <= p_.max_payload) {
                symbols_ = packet_symbols(payload_.data(), payload_.size(), seq);
                pos_ = 0;
                gap_ = kGapSymbols;
                ++packets_;
                a = symbols_[pos_++];
            }
        }
        next_symbol_samples(a);
    }
    std::copy(ready_.begin() + static_cast<std::ptrdiff_t>(ready_pos_),
              ready_.begin() + static_cast<std::ptrdiff_t>(ready_pos_ + 2 * n), iq);
    ready_pos_ += 2 * n;
    if (ready_pos_ > (1u << 16)) {
        ready_.erase(ready_.begin(), ready_.begin() + static_cast<std::ptrdiff_t>(ready_pos_));
        ready_pos_ = 0;
    }
}

// ---------------------------------------------------------------------------
// Demodulator

Demodulator::Demodulator(const Params& p)
    : p_(p), taps_(rrc_taps(p)), L_(kPreambleHalf * p.sps), D_(p.span * p.sps / 2) {
    // The preamble as it leaves the modulator (unit scale): the correlation template.
    const auto& pre = preamble();
    wave_.assign(pre.size() * p.sps + taps_.size() - 1, {});
    for (std::size_t k = 0; k < pre.size(); ++k) {
        for (std::size_t j = 0; j < taps_.size(); ++j) wave_[k * p.sps + j] += pre[k] * taps_[j];
    }
    for (const auto& w : wave_) wave_energy_ += std::norm(std::complex<double>(w));
    constellation_.reserve(kConstellationPoints);
}

void Demodulator::reset() {
    std::lock_guard lock(view_mutex_);
    ++stats_.resets;
    base_ = end();
    x_.clear();
    pos_ = base_;
    sums_ = false;
    state_ = State::search;
}

Stats Demodulator::stats() const {
    std::lock_guard lock(view_mutex_);
    return stats_;
}

std::vector<cf> Demodulator::constellation() const {
    std::lock_guard lock(view_mutex_);
    std::vector<cf> out(constellation_.begin() + static_cast<std::ptrdiff_t>(constellation_pos_), constellation_.end());
    out.insert(out.end(), constellation_.begin(),
               constellation_.begin() + static_cast<std::ptrdiff_t>(constellation_pos_));
    return out;
}

void Demodulator::restart_sums(std::int64_t n) {
    P_ = {};
    E1_ = E2_ = 0;
    for (unsigned m = 0; m < L_; ++m) {
        const std::complex<double> a(at(n + m)), b(at(n + m + L_));
        P_ += std::conj(a) * b;
        E1_ += std::norm(a);
        E2_ += std::norm(b);
    }
    sums_ = true;
}

void Demodulator::trim() {
    // Keep what the detector and a pending packet still need.
    const std::int64_t from = state_ == State::search ? pos_ : std::min(pos_, best_n_);
    const std::int64_t keep = from - static_cast<std::int64_t>(2 * L_ + 64);
    if (keep - base_ > (1 << 16)) {
        const auto drop = static_cast<std::size_t>(keep - base_);
        x_.erase(x_.begin(), x_.begin() + static_cast<std::ptrdiff_t>(drop));
        base_ += static_cast<std::int64_t>(drop);
    }
}

void Demodulator::push(const std::int16_t* iq, std::size_t n, const OnPacket& on_packet) {
    constexpr float k = 1.0f / 32768.0f;
    const std::size_t old = x_.size();
    x_.resize(old + n);
    for (std::size_t i = 0; i < n; ++i) x_[old + i] = cf(iq[2 * i] * k, iq[2 * i + 1] * k);
    {
        std::lock_guard lock(view_mutex_);
        stats_.samples += n;
    }

    const double thr = p_.detect_threshold;
    for (;;) {
        if (state_ == State::decode) {
            if (!try_decode(on_packet)) break;  // waits for more samples
            continue;
        }
        if (pos_ + 2 * L_ + 1 > end()) break;
        if (!sums_) restart_sums(pos_);
        // Schmidl & Cox: correlation of the two preamble halves, normalized.
        const double e = 0.5 * (E1_ + E2_);
        const double metric = e > 1e-12 ? std::norm(P_) / (e * e) : 0.0;
        if (state_ == State::search) {
            if (metric > thr) {
                state_ = State::peak;
                best_metric_ = metric;
                best_n_ = pos_;
                best_P_ = P_;
            }
        } else if (metric > best_metric_) {
            best_metric_ = metric;
            best_n_ = pos_;
            best_P_ = P_;
        } else if (metric < 0.7 * best_metric_ || pos_ > best_n_ + static_cast<std::int64_t>(L_ / 2)) {
            state_ = State::decode;
            {
                std::lock_guard lock(view_mutex_);
                ++stats_.detected;
            }
            continue;
        }
        // slide by one sample
        const std::complex<double> a(at(pos_)), b(at(pos_ + L_)), c(at(pos_ + 2 * L_));
        P_ += std::conj(b) * c - std::conj(a) * b;
        E1_ += std::norm(b) - std::norm(a);
        E2_ += std::norm(c) - std::norm(b);
        ++pos_;
        if ((pos_ & 0xFFFF) == 0) sums_ = false;  // recompute now and then: no drift
    }
    trim();
}

namespace {

/// Solve (A^H A + loading) x = A^H b by Gaussian elimination (small systems).
std::vector<std::complex<double>> least_squares(const std::vector<std::vector<std::complex<double>>>& rows,
                                                const std::vector<std::complex<double>>& b) {
    const std::size_t n = rows.empty() ? 0 : rows[0].size();
    std::vector<std::vector<std::complex<double>>> m(n, std::vector<std::complex<double>>(n + 1));
    for (std::size_t k = 0; k < rows.size(); ++k) {
        const auto& a = rows[k];
        for (std::size_t i = 0; i < n; ++i) {
            const auto ai = std::conj(a[i]);
            for (std::size_t j = 0; j < n; ++j) m[i][j] += ai * a[j];
            m[i][n] += ai * b[k];
        }
    }
    // Diagonal loading relative to each unknown's own scale: the feed-forward
    // inputs are as small as the received signal, the feedback ones are symbols.
    for (std::size_t i = 0; i < n; ++i) m[i][i] += 1e-3 * m[i][i].real() + 1e-12;
    for (std::size_t c = 0; c < n; ++c) {
        std::size_t piv = c;
        for (std::size_t i = c + 1; i < n; ++i) {
            if (std::abs(m[i][c]) > std::abs(m[piv][c])) piv = i;
        }
        std::swap(m[c], m[piv]);
        if (std::abs(m[c][c]) < 1e-30) continue;
        for (std::size_t i = c + 1; i < n; ++i) {
            const auto f = m[i][c] / m[c][c];
            for (std::size_t j = c; j <= n; ++j) m[i][j] -= f * m[c][j];
        }
    }
    std::vector<std::complex<double>> x(n);
    for (std::size_t i = n; i-- > 0;) {
        auto acc = m[i][n];
        for (std::size_t j = i + 1; j < n; ++j) acc -= m[i][j] * x[j];
        x[i] = std::abs(m[i][i]) < 1e-30 ? 0.0 : acc / m[i][i];
    }
    return x;
}

}  // namespace

bool Demodulator::try_decode(const OnPacket& on_packet) {
    const unsigned sps = p_.sps;
    // Equalizer geometry: os samples per symbol (T/2 when sps is even), nf
    // feed-forward taps of which `lead` look ahead, nb feedback taps (symbols).
    const unsigned os = (p_.equalizer && sps % 2 == 0) ? 2u : 1u;
    const unsigned hstep = sps / os;
    const int nf = p_.equalizer ? static_cast<int>(std::max(1u, p_.eq_ff)) : 1;
    const int lead = p_.equalizer ? static_cast<int>(std::min(p_.eq_lead, p_.eq_ff - 1)) : 0;
    const int nb = p_.equalizer ? static_cast<int>(p_.eq_fb) : 0;
    const int m_first = -(nf - 1 - lead);  // earliest equalizer sample, relative to symbol 0
    const std::size_t margin = static_cast<std::size_t>(-m_first) * hstep + 2;  // room before the packet
    const std::size_t ahead = static_cast<std::size_t>(lead) * hstep;           // and after a symbol

    const int W = static_cast<int>(6 * sps);  // timing search, samples either side
    // Where the preamble waveform starts, roughly: the S&C metric has a plateau
    // about 2 * D wide that starts there (the pulse tails spoil its edges).
    const std::int64_t guess = best_n_ - static_cast<std::int64_t>(D_);
    const std::int64_t z0 = guess - W - 2 - static_cast<std::int64_t>(margin);  // first sample used
    const double w = std::arg(best_P_) / L_;  // rad / sample
    const std::size_t hdr_end =
        margin + 2 * W + 8 + (kPreambleSymbols + kHeaderSymbols) * sps + ahead + taps_.size() + 2;
    if (z0 < base_) {  // the start was trimmed (a packet at the very start of the stream): give up on it
        state_ = State::search;
        sums_ = false;
        pos_ = std::max(pos_, best_n_ + static_cast<std::int64_t>(L_));
        return true;
    }
    if (z0 + static_cast<std::int64_t>(hdr_end) > end()) return false;

    // Frequency-corrected copy of the packet's samples (grown as needed below).
    std::vector<cf> z;
    float rr = 1, ri = 0;  // rotator exp(-j w i), multiplied out by hand
    const auto step = std::polar(1.0, -w);
    const float sr = static_cast<float>(step.real()), si = static_cast<float>(step.imag());
    auto take = [&](std::size_t upto) {
        const std::size_t from = z.size();
        z.resize(upto);
        const cf* in = &at(z0);
        for (std::size_t i = from; i < upto; ++i) {
            const float xr = in[i].real(), xi = in[i].imag();
            z[i] = cf(xr * rr - xi * ri, xr * ri + xi * rr);
            const float nr = rr * sr - ri * si;
            ri = rr * si + ri * sr;
            rr = nr;
            if ((i & 255) == 255) {  // keep |rotator| = 1
                const float g = 1.0f / std::sqrt(rr * rr + ri * ri);
                rr *= g;
                ri *= g;
            }
        }
    };
    take(hdr_end);

    // Timing: correlate with the preamble waveform, parabola through the peak.
    double best = -1;
    int best_d = 0;
    std::vector<double> mag(2 * W + 3);
    for (int d = 0; d <= 2 * W + 2; ++d) {
        // sum conj(wave) * z, written out (std::complex multiplication checks for NaNs)
        float re = 0, im = 0;
        const cf* in = z.data() + margin + d;
        for (std::size_t m = 0; m < wave_.size(); ++m) {
            const cf a = wave_[m], b = in[m];
            re += a.real() * b.real() + a.imag() * b.imag();
            im += a.real() * b.imag() - a.imag() * b.real();
        }
        mag[d] = std::hypot(re, im);
        if (mag[d] > best) {
            best = mag[d];
            best_d = d;
        }
    }
    double frac = 0;
    if (best_d > 0 && best_d < 2 * W + 2) {
        const double a = mag[best_d - 1], b = mag[best_d], c = mag[best_d + 1];
        const double den = a - 2 * b + c;
        if (den < 0) frac = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    }
    const double start = static_cast<double>(margin) + best_d + frac;  // packet waveform start in z, samples

    // Matched filter at a fractional instant: taps shifted by the packet's fraction.
    const double t0 = start + D_;  // centre of symbol 0
    const auto i0 = static_cast<std::int64_t>(std::floor(t0));
    const double f = t0 - static_cast<double>(i0);
    const double norm = rrc_norm(p_);
    std::vector<float> hf(taps_.size() + 1);
    for (std::size_t q = 0; q < hf.size(); ++q) {
        hf[q] = static_cast<float>(norm * rrc((static_cast<double>(q) - D_ - f) / sps, p_.rolloff));
    }
    // Matched filter outputs every hstep samples: r[m] at symbol 0 + m * hstep (m >= m_first).
    std::vector<cf> r;
    auto r_upto = [&](int m_last) {
        for (int m = m_first + static_cast<int>(r.size()); m <= m_last; ++m) {
            const std::int64_t c = i0 + static_cast<std::int64_t>(m) * hstep - static_cast<std::int64_t>(D_);
            const cf* in = z.data() + c;
            float re = 0, im = 0;
            for (std::size_t q = 0; q < hf.size(); ++q) {
                re += hf[q] * in[q].real();
                im += hf[q] * in[q].imag();
            }
            r.emplace_back(re, im);
        }
    };
    auto rv = [&](int m) { return r[static_cast<std::size_t>(m - m_first)]; };
    const int pre_hdr = static_cast<int>(kPreambleSymbols + kHeaderSymbols);
    r_upto(static_cast<int>(os) * (pre_hdr - 1) + lead);

    // Level: the plain symbols correlated with the preamble.
    const auto& pre = preamble();
    std::complex<double> g{};
    for (std::size_t k = 0; k < pre.size(); ++k) {
        g += std::complex<double>(rv(static_cast<int>(os * k))) * std::conj(std::complex<double>(pre[k]));
    }
    g /= static_cast<double>(pre.size());
    if (std::abs(g) < 1e-9) {
        state_ = State::search;
        sums_ = false;
        pos_ = best_n_ + L_;
        return true;
    }

    // Decided (or known) symbols; feedback taps read the past ones.
    std::vector<cf> dec;
    dec.reserve(static_cast<std::size_t>(pre_hdr) + 4096);
    auto past = [&](int k) { return k >= 0 ? dec[static_cast<std::size_t>(k)] : cf{}; };

    // Equalizer trained by least squares on known symbols: it takes out gain,
    // phase and echoes at once (with nf = 1, nb = 0 it is just 1 / gain).
    // `corr[k]` is the loops' correction at symbol k (1 for the first training).
    std::vector<cf> wf(static_cast<std::size_t>(nf)), wb(static_cast<std::size_t>(nb));
    std::vector<cf> corr;
    auto train = [&](int count) {
        std::vector<std::vector<std::complex<double>>> rows;
        std::vector<std::complex<double>> want;
        for (int k = 0; k < count; ++k) {
            const std::complex<double> c = k < static_cast<int>(corr.size()) ? std::complex<double>(corr[k]) : 1.0;
            std::vector<std::complex<double>> a;
            for (int j = 0; j < nf; ++j) a.push_back(c * std::complex<double>(rv(static_cast<int>(os) * k + lead - j)));
            for (int j = 1; j <= nb; ++j) a.push_back(-std::complex<double>(past(k - j)));
            rows.push_back(std::move(a));
            want.emplace_back(dec[k]);
        }
        const auto x = least_squares(rows, want);
        for (int j = 0; j < nf; ++j) wf[j] = cf(x[j]);
        for (int j = 0; j < nb; ++j) wb[j] = cf(x[nf + j]);
    };
    dec.assign(pre.begin(), pre.end());
    train(static_cast<int>(kPreambleSymbols));
    dec.clear();

    // After the equalizer: a phase loop (2nd order: phase and residual frequency)
    // and a gain loop, both decision directed, so fading and drift inside the
    // packet are followed; the equalizer taps adapt slowly (normalized LMS).
    cf derot(1, 0);
    float freq = 0, amp = 1;
    unsigned tracked = 0;
    const float kp = 0.06f, ki = 0.002f, ka = 0.03f;
    // Taps adapt every kEvery symbols (fast changes are the loops' job): less CPU.
    constexpr int kEvery = 4;
    const float mu_f = p_.equalizer ? 0.003f * kEvery : 0.0f, mu_b = p_.equalizer ? 0.002f * kEvery : 0.0f;
    // One symbol: equalizer output, then the loops and the taps follow the decision.
    auto output = [&](int k) {
        cf ff{};
        for (int j = 0; j < nf; ++j) ff += wf[j] * rv(static_cast<int>(os) * k + lead - j);
        cf fb{};
        for (int j = 1; j <= nb; ++j) fb += wb[j - 1] * past(k - j);
        return ff * (derot * amp) - fb;
    };
    auto adapt = [&](int k, cf v, cf d) {
        const float e2 = std::norm(d);
        // phase and gain of v relative to the decision
        const cf ratio = v * std::conj(d) / e2;  // ~ (1 + gain error) * exp(j phase error)
        const float pe = ratio.imag(), ae = ratio.real() - 1;
        const cf e = d - v;
        if (mu_f > 0 && k % kEvery == 0) {  // taps: before the loops change c
            const cf c = derot * amp;
            float power = 1e-6f;
            for (int j = 0; j < nf; ++j) power += std::norm(rv(static_cast<int>(os) * k + lead - j));
            const cf g_f = e * std::conj(c) * (mu_f / (power * std::norm(c)));
            for (int j = 0; j < nf; ++j) wf[j] += g_f * std::conj(rv(static_cast<int>(os) * k + lead - j));
            for (int j = 1; j <= nb; ++j) wb[j - 1] -= (mu_b / static_cast<float>(nb)) * e * std::conj(past(k - j));
        }
        corr.push_back(derot * amp);  // the correction symbol k was given
        freq += ki * pe;
        const float dp = freq + kp * pe;
        derot *= cf(1 - 0.5f * dp * dp, -dp);
        amp *= 1 - ka * std::clamp(ae, -0.5f, 0.5f);
        if ((++tracked & 31) == 0) derot /= std::abs(derot);
    };

    for (int k = 0; k < static_cast<int>(kPreambleSymbols); ++k) {  // training: decisions are known
        adapt(k, output(k), pre[k]);
        dec.push_back(pre[k]);
    }

    // Header
    std::uint8_t h[6] = {};
    for (std::size_t s = 0; s < kHeaderSymbols; ++s) {
        const int k = static_cast<int>(kPreambleSymbols + s);
        const cf v = output(k);
        const unsigned bits = qpsk_bits(v);
        adapt(k, v, qpsk(bits));
        dec.push_back(qpsk(bits));
        h[s / 4] = static_cast<std::uint8_t>((h[s / 4] << 2) | bits);
    }
    const std::size_t len = (std::size_t(h[0]) << 8) | h[1];
    const std::uint16_t seq = static_cast<std::uint16_t>((h[2] << 8) | h[3]);
    if (crc16(h, 4) != ((std::uint16_t(h[4]) << 8) | h[5]) || len == 0 || len > p_.max_payload) {
        std::lock_guard lock(view_mutex_);
        ++stats_.header_errors;
        state_ = State::search;
        sums_ = false;
        pos_ = best_n_ + L_;
        return true;
    }

    // The header is known now (its CRC holds): train again on 88 symbols
    // instead of 64, with the loops' corrections as they were: less noise in
    // the taps, and the loops carry on from where they are.
    if (p_.equalizer) train(pre_hdr);

    // Payload: wait for all of it.
    const std::size_t nsym = (len + 4) * 2;
    const std::size_t need =
        static_cast<std::size_t>(i0) + (kPreambleSymbols + kHeaderSymbols + nsym) * sps + ahead + hf.size() + 2;
    if (z0 + static_cast<std::int64_t>(need) > end()) return false;
    take(need);
    r_upto(static_cast<int>(os) * (pre_hdr + static_cast<int>(nsym) - 1) + lead);

    std::vector<std::uint8_t> body(len + 4);
    double err_power = 0;
    std::vector<cf> points;
    points.reserve(nsym);
    for (std::size_t s = 0; s < nsym; ++s) {
        const int k = pre_hdr + static_cast<int>(s);
        const cf v = output(k);
        const unsigned nib = demap16(v);
        const cf d = map16(nib);
        adapt(k, v, d);
        dec.push_back(d);
        err_power += std::norm(v - d);
        points.push_back(v);
        body[s / 2] = static_cast<std::uint8_t>((body[s / 2] << 4) | nib);
    }
    scramble(body.data(), body.size());
    const std::uint32_t got = std::uint32_t(body[len]) | std::uint32_t(body[len + 1]) << 8 |
                              std::uint32_t(body[len + 2]) << 16 | std::uint32_t(body[len + 3]) << 24;
    const bool ok = crc32(body.data(), len) == got;

    Packet pkt;
    pkt.seq = seq;
    pkt.evm_db = static_cast<float>(10 * std::log10(std::max(err_power / static_cast<double>(nsym), 1e-12)));
    pkt.cfo_hz = static_cast<float>(w * p_.sample_rate / (2 * kPi));
    pkt.level_db = static_cast<float>(20 * std::log10(std::max(std::abs(g), 1e-12)));
    pkt.timing = static_cast<double>(z0) + start;
    {
        std::lock_guard lock(view_mutex_);
        if (ok) {
            ++stats_.packets;
        } else {
            ++stats_.crc_errors;
        }
        const std::size_t step = std::max<std::size_t>(1, points.size() / 128);  // a sample of each packet
        for (std::size_t i = 0; i < points.size(); i += step) {
            if (constellation_.size() < kConstellationPoints) {
                constellation_.push_back(points[i]);
            } else {
                constellation_[constellation_pos_] = points[i];
                constellation_pos_ = (constellation_pos_ + 1) % kConstellationPoints;
            }
        }
    }
    // Continue after the packet.
    pos_ = z0 + static_cast<std::int64_t>(need) - static_cast<std::int64_t>(hf.size());
    sums_ = false;
    state_ = State::search;
    if (ok) {
        body.resize(len);
        pkt.payload = std::move(body);
        on_packet(std::move(pkt));
    }
    return true;
}

}  // namespace qam
