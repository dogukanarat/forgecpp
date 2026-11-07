#pragma once
// Small DSP helpers for monitoring (not for the data path).

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace radio::dsp {

/// In-place radix-2 FFT; size must be a power of two.
void fft(std::vector<std::complex<float>>& x);

/// Power spectrum in dBFS of `n` cs16 samples (Hann window), DC in the
/// middle (index n/2), negative frequencies first. `n` must be a power of two.
std::vector<float> spectrum_dbfs(const std::int16_t* iq, std::size_t n);

/// RMS power of cs16 samples in dBFS (0 dBFS = full-scale sine).
double power_dbfs(const std::int16_t* iq, std::size_t n);

/// Largest bin of a spectrum and its offset from the centre in Hz.
struct Peak {
    double offset_hz = 0;
    float dbfs = -200;
};
Peak find_peak(const std::vector<float>& spectrum, double sample_rate_hz);

bool is_power_of_two(std::size_t n);

}  // namespace radio::dsp
