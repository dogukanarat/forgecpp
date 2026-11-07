#include "dsp.hpp"

#include <algorithm>
#include <cmath>

namespace radio::dsp {

bool is_power_of_two(std::size_t n) { return n >= 2 && (n & (n - 1)) == 0; }

void fft(std::vector<std::complex<float>>& x) {
    const std::size_t n = x.size();
    // bit reversal
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / static_cast<double>(len);
        const std::complex<float> wlen(static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang)));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = x[i + k];
                const auto v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

std::vector<float> spectrum_dbfs(const std::int16_t* iq, std::size_t n) {
    std::vector<std::complex<float>> x(n);
    double window_sum = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * M_PI * static_cast<double>(i) / static_cast<double>(n));
        window_sum += w;
        x[i] = {static_cast<float>(iq[2 * i] / 32768.0 * w), static_cast<float>(iq[2 * i + 1] / 32768.0 * w)};
    }
    fft(x);
    std::vector<float> out(n);
    const double norm = 1.0 / window_sum;  // a full-scale complex tone reads 0 dBFS
    for (std::size_t i = 0; i < n; ++i) {
        const auto& v = x[(i + n / 2) % n];  // fftshift
        const double mag = std::abs(std::complex<double>(v.real(), v.imag())) * norm;
        out[i] = static_cast<float>(20 * std::log10(std::max(mag, 1e-10)));
    }
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
