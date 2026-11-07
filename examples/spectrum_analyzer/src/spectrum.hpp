#pragma once
// Spectrum analyzer measurement core: sweep planning, detectors, traces,
// peaks and power measurements. Pure functions, no I/O.

#include "dsp.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace sa {

// --- sweep planning ----------------------------------------------------------

struct HardwareLimits {
    double lo_min_hz = 70e6;
    double lo_max_hz = 6000e6;
    double rate_min_hz = 2.083334e6;
    double rate_max_hz = 61.44e6;     ///< fastest rate for spans that fit in one capture
    double sweep_rate_hz = 61.44e6;   ///< rate used for swept spans
    double usable_fraction = 0.8;     ///< part of the band used (filter roll-off at the edges)
    std::size_t fft_min = 64;
    std::size_t fft_max = 65536;
};

struct Segment {
    double lo_hz;      ///< LO for this capture
    double from_hz;    ///< usable part of the capture
    double to_hz;
};

struct Plan {
    double start_hz = 0, stop_hz = 0;
    double sample_rate_hz = 0;
    double rf_bandwidth_hz = 0;
    std::size_t fft_size = 0;
    double rbw_hz = 0;        ///< actual resolution bandwidth (window ENBW * bin width)
    double enbw_bins = 1;
    bool swept = false;
    std::vector<Segment> segments;
};

/// Plan captures for [center - span/2, center + span/2]. rbw_hz <= 0 selects
/// an automatic RBW (about span / 500).
Plan make_plan(double center_hz, double span_hz, double rbw_hz, radio::dsp::Window window,
               const HardwareLimits& limits = {});

// --- bins, detectors, traces -------------------------------------------------

/// FFT bins of one sweep, sorted by frequency (linear power, dBFS reference).
struct Bins {
    std::vector<double> freq_hz;
    std::vector<double> power;
    void clear() {
        freq_hz.clear();
        power.clear();
    }
};

/// Append the bins of a capture (fftshifted linear power at `lo_hz`) that fall in [from, to).
void append_bins(Bins& bins, const std::vector<double>& spectrum, double lo_hz, double sample_rate_hz,
                 double from_hz, double to_hz);

enum class Detector { peak, sample, average, negative_peak };
Detector parse_detector(const std::string& name);
const std::vector<std::string>& detector_names();

/// Reduce (or interpolate) bins to `points` display points over [start, stop], in dB.
std::vector<double> to_display(const Bins& bins, double start_hz, double stop_hz, std::size_t points,
                               Detector detector);

enum class TraceMode { clear_write, max_hold, min_hold, average, view, blank };
TraceMode parse_trace_mode(const std::string& name);
const std::vector<std::string>& trace_mode_names();

class Trace {
public:
    void set_mode(TraceMode m);
    TraceMode mode() const { return mode_; }
    void reset();
    /// Feed one sweep (dB). `average_count` is used in average mode.
    void update(const std::vector<double>& db, std::size_t average_count);
    const std::vector<double>& db() const { return db_; }
    bool visible() const { return mode_ != TraceMode::blank && !db_.empty(); }
    std::size_t sweeps() const { return count_; }

private:
    TraceMode mode_ = TraceMode::clear_write;
    std::vector<double> db_;
    std::vector<double> linear_;  ///< running average in linear power
    std::size_t count_ = 0;
};

// --- peaks and measurements ----------------------------------------------------

struct PeakPoint {
    std::size_t index;
    double freq_hz;
    double db;
};

/// Local maxima above `threshold_db` that stand out by at least `excursion_db`
/// from the surrounding valleys, strongest first, at most `max_peaks`.
std::vector<PeakPoint> find_peaks(const std::vector<double>& db, double start_hz, double stop_hz,
                                  double threshold_db, double excursion_db, std::size_t max_peaks);

/// Frequency of display point i.
inline double point_freq(std::size_t i, double start_hz, double stop_hz, std::size_t points) {
    return points < 2 ? (start_hz + stop_hz) / 2
                      : start_hz + (stop_hz - start_hz) * static_cast<double>(i) / static_cast<double>(points - 1);
}

/// Index of the display point closest to `freq_hz`.
std::size_t point_index(double freq_hz, double start_hz, double stop_hz, std::size_t points);

/// Total power (dB) in [from, to], integrating bins (noise and signals alike).
double channel_power_db(const Bins& bins, double from_hz, double to_hz, double enbw_bins);

/// Bandwidth containing `fraction` (e.g. 0.99) of the power in the bins.
struct Occupied {
    double bandwidth_hz = 0;
    double from_hz = 0, to_hz = 0;
};
Occupied occupied_bandwidth(const Bins& bins, double fraction);

}  // namespace sa
