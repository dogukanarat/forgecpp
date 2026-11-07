#pragma once
// DSP helpers for monitoring and spectrum analysis.

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace radio::dsp {

bool is_power_of_two(std::size_t n);

/// Radix-2 FFT with precomputed twiddles and bit-reversal table.
/// Create once per size, use from one thread at a time.
class Fft {
public:
    explicit Fft(std::size_t n);  ///< n: power of two >= 2
    std::size_t size() const { return n_; }
    void forward(std::complex<float>* data) const;

private:
    std::size_t n_;
    std::vector<std::complex<float>> twiddles_;  ///< e^{-2 pi i k / n}, k < n/2
    std::vector<std::uint32_t> reversed_;
};

/// In-place FFT (convenience; builds a plan each call).
void fft(std::vector<std::complex<float>>& x);

enum class Window { rectangular, hann, hamming, blackman_harris, flat_top };

Window parse_window(const std::string& name);  ///< throws std::invalid_argument
const char* to_string(Window w);
const std::vector<std::string>& window_names();

struct WindowInfo {
    std::vector<float> coefficients;
    double coherent_gain = 1;  ///< sum(w) / n
    double enbw_bins = 1;      ///< equivalent noise bandwidth in bins: n * sum(w^2) / sum(w)^2
};
WindowInfo make_window(Window w, std::size_t n);

/// Power spectrum of `ffts` consecutive FFTs (ffts * fft.size() cs16 samples), averaged in
/// linear power and normalised so a full-scale complex tone reads 0 dBFS.
/// Output is linear power (not dB), fftshifted (DC at n/2).
void power_spectrum(const std::int16_t* iq, std::size_t ffts, const Fft& fft, const WindowInfo& window,
                    std::vector<double>& out);

/// Power spectrum in dBFS of `n` cs16 samples (Hann window), DC in the
/// middle (index n/2), negative frequencies first. `n` must be a power of two.
std::vector<float> spectrum_dbfs(const std::int16_t* iq, std::size_t n);

/// RMS power of cs16 samples in dBFS (0 dBFS = full-scale complex sine).
double power_dbfs(const std::int16_t* iq, std::size_t n);

/// Largest |I| or |Q| relative to full scale (1.0 = clipping).
double peak_magnitude(const std::int16_t* iq, std::size_t n);

/// Largest bin of a spectrum and its offset from the centre in Hz.
struct Peak {
    double offset_hz = 0;
    float dbfs = -200;
};
Peak find_peak(const std::vector<float>& spectrum, double sample_rate_hz);

inline double to_db(double power) { return power > 1e-30 ? 10.0 * std::log10(power) : -300.0; }
inline double from_db(double db) { return std::pow(10.0, db / 10.0); }

}  // namespace radio::dsp
