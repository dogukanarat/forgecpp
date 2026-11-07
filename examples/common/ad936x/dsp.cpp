#include "dsp.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace radio::dsp {

bool is_power_of_two(std::size_t n) { return n >= 2 && (n & (n - 1)) == 0; }

// ---------------------------------------------------------------------------
// FFT

Fft::Fft(std::size_t n) : n_(n) {
    if (!is_power_of_two(n)) throw std::invalid_argument("FFT size must be a power of two");
    twiddles_.resize(n / 2);
    for (std::size_t k = 0; k < n / 2; ++k) {
        const double a = -2.0 * M_PI * static_cast<double>(k) / static_cast<double>(n);
        twiddles_[k] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
    }
    reversed_.resize(n);
    unsigned bits = 0;
    while ((std::size_t{1} << bits) < n) ++bits;
    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t r = 0;
        for (unsigned b = 0; b < bits; ++b) r |= ((i >> b) & 1u) << (bits - 1 - b);
        reversed_[i] = r;
    }
}

void Fft::forward(std::complex<float>* x) const {
    for (std::size_t i = 0; i < n_; ++i) {
        if (i < reversed_[i]) std::swap(x[i], x[reversed_[i]]);
    }
    for (std::size_t len = 2; len <= n_; len <<= 1) {
        const std::size_t half = len / 2;
        const std::size_t stride = n_ / len;
        for (std::size_t i = 0; i < n_; i += len) {
            for (std::size_t k = 0; k < half; ++k) {
                const auto v = x[i + k + half] * twiddles_[k * stride];
                const auto u = x[i + k];
                x[i + k] = u + v;
                x[i + k + half] = u - v;
            }
        }
    }
}

void fft(std::vector<std::complex<float>>& x) { Fft(x.size()).forward(x.data()); }

// ---------------------------------------------------------------------------
// Windows

const std::vector<std::string>& window_names() {
    static const std::vector<std::string> names{"rectangular", "hann", "hamming", "blackman_harris", "flat_top"};
    return names;
}

const char* to_string(Window w) {
    switch (w) {
        case Window::rectangular: return "rectangular";
        case Window::hann: return "hann";
        case Window::hamming: return "hamming";
        case Window::blackman_harris: return "blackman_harris";
        case Window::flat_top: return "flat_top";
    }
    return "hann";
}

Window parse_window(const std::string& name) {
    if (name == "rectangular") return Window::rectangular;
    if (name == "hann") return Window::hann;
    if (name == "hamming") return Window::hamming;
    if (name == "blackman_harris") return Window::blackman_harris;
    if (name == "flat_top") return Window::flat_top;
    throw std::invalid_argument("unknown window '" + name + "'");
}

WindowInfo make_window(Window w, std::size_t n) {
    WindowInfo info;
    info.coefficients.resize(n);
    // Generalised cosine windows: w[i] = sum_k (-1)^k a_k cos(2 pi k i / N) (periodic form).
    std::vector<double> a;
    switch (w) {
        case Window::rectangular: a = {1.0}; break;
        case Window::hann: a = {0.5, 0.5}; break;
        case Window::hamming: a = {0.54, 0.46}; break;
        case Window::blackman_harris: a = {0.35875, 0.48829, 0.14128, 0.01168}; break;
        case Window::flat_top: a = {0.21557895, 0.41663158, 0.277263158, 0.083578947, 0.006947368}; break;
    }
    double sum = 0, sum2 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        double v = 0;
        for (std::size_t k = 0; k < a.size(); ++k) {
            const double sign = (k % 2) ? -1.0 : 1.0;
            v += sign * a[k] * std::cos(2.0 * M_PI * static_cast<double>(k * i) / static_cast<double>(n));
        }
        info.coefficients[i] = static_cast<float>(v);
        sum += v;
        sum2 += v * v;
    }
    info.coherent_gain = sum / static_cast<double>(n);
    info.enbw_bins = static_cast<double>(n) * sum2 / (sum * sum);
    return info;
}

// ---------------------------------------------------------------------------
// Spectra

void power_spectrum(const std::int16_t* iq, std::size_t frames, const Fft& fft, const WindowInfo& window,
                    std::vector<double>& out) {
    const std::size_t n = fft.size();
    out.assign(n, 0.0);
    std::vector<std::complex<float>> x(n);
    // Normalisation: a full-scale complex tone (amplitude 1) has |X| = sum(w).
    const double sum_w = window.coherent_gain * static_cast<double>(n);
    const double norm = 1.0 / (sum_w * sum_w * static_cast<double>(std::max<std::size_t>(frames, 1)));
    for (std::size_t f = 0; f < frames; ++f) {
        const std::int16_t* p = iq + 2 * f * n;
        for (std::size_t i = 0; i < n; ++i) {
            const float w = window.coefficients[i] / 32768.0f;
            x[i] = {p[2 * i] * w, p[2 * i + 1] * w};
        }
        fft.forward(x.data());
        for (std::size_t i = 0; i < n; ++i) {
            const auto& v = x[(i + n / 2) % n];  // fftshift
            out[i] += (static_cast<double>(v.real()) * v.real() + static_cast<double>(v.imag()) * v.imag()) * norm;
        }
    }
}

std::vector<float> spectrum_dbfs(const std::int16_t* iq, std::size_t n) {
    const Fft fft(n);
    const auto window = make_window(Window::hann, n);
    std::vector<double> p;
    power_spectrum(iq, 1, fft, window, p);
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(std::max(to_db(p[i]), -200.0));
    return out;
}

double power_dbfs(const std::int16_t* iq, std::size_t n) {
    if (n == 0) return -200;
    double sum = 0;
    for (std::size_t i = 0; i < 2 * n; ++i) {
        const double v = iq[i] / 32768.0;
        sum += v * v;
    }
    return 10 * std::log10(std::max(sum / static_cast<double>(n), 1e-20));
}

double peak_magnitude(const std::int16_t* iq, std::size_t n) {
    int peak = 0;
    for (std::size_t i = 0; i < 2 * n; ++i) peak = std::max(peak, std::abs(static_cast<int>(iq[i])));
    return peak / 32768.0;
}

Peak find_peak(const std::vector<float>& spectrum, double sample_rate_hz) {
    Peak p;
    if (spectrum.empty()) return p;
    const auto it = std::max_element(spectrum.begin(), spectrum.end());
    const auto i = static_cast<double>(std::distance(spectrum.begin(), it));
    const auto n = static_cast<double>(spectrum.size());
    p.dbfs = *it;
    p.offset_hz = (i - n / 2) * sample_rate_hz / n;
    return p;
}

}  // namespace radio::dsp
