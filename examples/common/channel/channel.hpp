// Channel impairments for complex baseband samples: what a radio link does to
// a signal, applied digitally (channel-emulator, tests).
//
// Plain C++ without forgecpp. One Channel processes one continuous stream
// (keeps its filters, oscillators and fading state between blocks).
//
// Order (each stage on/off):
//
//   clock offset -> multipath -> fading -> gain -> frequency offset/Doppler
//   -> phase noise -> IQ imbalance -> DC offset -> interference -> noise
//
// Levels of the added things (noise, DC, interference) are relative to the
// signal: its power is measured while it is present (samples that are not
// zero), so a bursty signal (packets with gaps) gets the SNR asked for while a
// packet is on the air, whatever the TX level.
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace channel {

using cf = std::complex<float>;

/// One propagation path of the multipath profile.
struct Path {
    double delay_us = 0;
    double gain_db = 0;
    double phase_deg = 0;
};

struct Settings {
    bool bypass = false;  ///< pass the samples through untouched

    double gain_db = 0;  ///< signal gain (path loss); SNR, DC and interference follow the signal

    bool frequency = false;
    double cfo_hz = 0;             ///< constant frequency offset
    double doppler_hz = 0;         ///< + sinusoidal Doppler of this amplitude ...
    double doppler_period_s = 10;  ///< ... and period (a pass overhead, a car going round)

    bool phase_noise = false;
    double linewidth_hz = 100;  ///< Wiener phase noise (oscillator linewidth)

    bool clock = false;
    double clock_ppm = 0;  ///< sample clock error of the transmitter (+: faster)

    bool multipath = false;
    std::vector<Path> paths;  ///< power normalized to 1 over all paths

    bool fading = false;
    double fading_hz = 10;       ///< maximum Doppler of the scatterers (Jakes spectrum)
    double rician_k_db = -100;   ///< direct path / scattered power; very low = Rayleigh

    bool iq = false;
    double iq_gain_db = 0;      ///< I/Q amplitude imbalance
    double iq_phase_deg = 0;    ///< I/Q quadrature error

    bool dc = false;
    double dc_dbc = -30;        ///< DC offset relative to the signal
    double dc_phase_deg = 45;

    bool interference = false;
    double interference_hz = 400e3;  ///< tone offset from the centre
    double interference_dbc = -10;   ///< tone power relative to the signal
    double interference_on_ms = 0;   ///< pulsed: on for this long ...
    double interference_off_ms = 0;  ///< ... off for this long (0 = continuous)

    bool noise = false;
    double snr_db = 30;
    /// Bandwidth the SNR is defined in (the noise is white over the sample
    /// rate): 0 = the whole sample rate; the symbol rate = Es/N0 of a modem.
    double noise_bandwidth_hz = 0;
};

struct Stats {
    std::uint64_t samples = 0;
    std::uint64_t clipped = 0;         ///< output samples beyond full scale (cs16)
    std::uint64_t timing_resets = 0;   ///< clock offset: delay reset in a quiet gap
    std::uint64_t timing_forced = 0;   ///< ... or forced in the middle of a signal
    double signal_dbfs = -200;         ///< measured signal power (while present)
};

class Channel {
public:
    explicit Channel(double sample_rate, std::uint64_t seed = 1);

    /// New settings; oscillators, filters and fading keep running where they can.
    void configure(const Settings& s);
    const Settings& settings() const { return s_; }
    void set_sample_rate(double hz);
    double sample_rate() const { return rate_; }

    /// Process `n` interleaved int16 I/Q samples in place (full scale 32767).
    void process(std::int16_t* iq, std::size_t n);
    /// Process normalized samples (full scale 1) in place.
    void process(cf* x, std::size_t n);

    Stats stats() const { return stats_; }
    /// Measured signal power (full scale 1 = 0 dBFS), linear.
    double signal_power() const { return p_signal_; }

private:
    void rebuild();
    void stage_clock(cf* x, std::size_t n);
    void stage_multipath(cf* x, std::size_t n);
    void stage_fading(cf* x, std::size_t n);
    void stage_frequency(cf* x, std::size_t n);
    float gauss();

    Settings s_;
    double rate_;
    Stats stats_;
    double p_signal_ = -1;  // < 0: not measured yet

    // random numbers: xorshift + Gaussian table
    std::uint64_t rng_;
    std::uint32_t next_u32();

    // clock offset: fractional delay (polyphase windowed sinc) that drifts
    std::vector<cf> clock_hist_;  // input history
    double tau_ = 0;              // extra delay, samples
    std::size_t quiet_run_ = 0;   // input samples in a row that were zero

    // multipath FIR
    std::vector<cf> mp_taps_, mp_hist_;

    // fading: sum of sinusoids (rotating phasors)
    std::vector<cf> fade_phasor_, fade_step_;
    cf fade_los_{1, 0}, fade_los_step_{1, 0};
    cf fade_cur_{1, 0}, fade_delta_{0, 0};
    float fade_los_amp_ = 0, fade_scatter_amp_ = 1;
    std::size_t fade_pos_ = 0;       // samples since the last update
    std::uint64_t fade_updates_ = 0;
    double fading_hz_built_ = -1;    // the phasors were set up for this Doppler and rate
    double fading_rate_built_ = -1;

    // oscillators (double phase, so long runs do not drift)
    double freq_phase_ = 0;    // CFO/Doppler, rad
    double doppler_t_ = 0;     // s
    double pn_phase_ = 0;      // phase noise, rad
    double tone_phase_ = 0;    // interference, rad
    double tone_t_ = 0;        // s, for pulsing
};

/// Ready-made settings by name ("clean", "awgn-20", "mobile", "urban", ...).
/// Unknown names throw std::invalid_argument.
Settings preset(const std::string& name);
std::vector<std::string> preset_names();

}  // namespace channel
