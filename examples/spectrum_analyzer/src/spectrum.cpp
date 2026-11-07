#include "spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sa {

using radio::dsp::from_db;
using radio::dsp::to_db;

namespace {
std::size_t next_pow2(double v) {
    std::size_t n = 1;
    while (static_cast<double>(n) < v) n <<= 1;
    return n;
}

// RBW -> FFT size: RBW = ENBW(bins) * rate / N (the power of two that reaches the target RBW).
void choose_fft(Plan& p, double span_hz, double rbw_hz, radio::dsp::Window window, std::size_t max_fft,
                const HardwareLimits& lim) {
    std::size_t cap = lim.fft_min;
    while (cap * 2 <= std::min(max_fft, lim.fft_max)) cap *= 2;
    const double enbw = radio::dsp::make_window(window, 64).enbw_bins;
    const double target = rbw_hz > 0 ? rbw_hz : span_hz / 500.0;
    p.fft_size = std::clamp(next_pow2(enbw * p.sample_rate_hz / target), lim.fft_min, cap);
    p.enbw_bins = radio::dsp::make_window(window, p.fft_size).enbw_bins;
    p.rbw_hz = p.enbw_bins * p.sample_rate_hz / static_cast<double>(p.fft_size);
}
}  // namespace

Plan make_fixed_plan(double center_hz, double span_hz, double rbw_hz, radio::dsp::Window window, double lo_hz,
                     double rate_hz, std::size_t max_fft, const HardwareLimits& lim) {
    Plan p;
    p.fixed = true;
    p.sample_rate_hz = rate_hz;
    const double usable = rate_hz * lim.usable_fraction;
    const double low = lo_hz - usable / 2, high = lo_hz + usable / 2;
    span_hz = std::clamp(span_hz, std::min(1e3, usable), usable);
    if (center_hz + span_hz / 2 <= low || center_hz - span_hz / 2 >= high) {
        center_hz = lo_hz;  // nothing of the requested window is in the band
        span_hz = usable;
    }
    center_hz = std::clamp(center_hz, low + span_hz / 2, high - span_hz / 2);
    p.start_hz = center_hz - span_hz / 2;
    p.stop_hz = center_hz + span_hz / 2;
    choose_fft(p, span_hz, rbw_hz, window, max_fft, lim);
    p.segments.push_back({lo_hz, p.start_hz, p.stop_hz});
    return p;
}

Plan make_plan(double center_hz, double span_hz, double rbw_hz, radio::dsp::Window window,
               const HardwareLimits& lim) {
    Plan p;
    span_hz = std::max(span_hz, 1e3);
    p.start_hz = center_hz - span_hz / 2;
    p.stop_hz = center_hz + span_hz / 2;

    const double realtime_max = lim.rate_max_hz * lim.usable_fraction;
    p.swept = span_hz > realtime_max;
    p.sample_rate_hz = p.swept ? lim.sweep_rate_hz : std::clamp(span_hz / lim.usable_fraction, lim.rate_min_hz,
                                                                 lim.rate_max_hz);
    p.rf_bandwidth_hz = std::min(p.sample_rate_hz * lim.usable_fraction * 1.05, 56e6);

    choose_fft(p, span_hz, rbw_hz, window, lim.fft_max, lim);

    if (!p.swept) {
        p.segments.push_back({center_hz, p.start_hz, p.stop_hz});
        return p;
    }
    const double step = p.sample_rate_hz * lim.usable_fraction;
    for (double from = p.start_hz; from < p.stop_hz; from += step) {
        const double to = std::min(from + step, p.stop_hz);
        double lo = from + step / 2;
        lo = std::clamp(lo, lim.lo_min_hz, lim.lo_max_hz);
        p.segments.push_back({lo, from, to});
    }
    return p;
}

void append_bins(Bins& bins, const std::vector<double>& spectrum, double lo_hz, double rate, double from_hz,
                 double to_hz) {
    const auto n = spectrum.size();
    const double bin = rate / static_cast<double>(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double f = lo_hz + (static_cast<double>(i) - static_cast<double>(n) / 2) * bin;
        if (f < from_hz || f >= to_hz) continue;
        bins.freq_hz.push_back(f);
        bins.power.push_back(spectrum[i]);
    }
}

// --- detectors ------------------------------------------------------------------

const std::vector<std::string>& detector_names() {
    static const std::vector<std::string> n{"peak", "sample", "average", "negative_peak"};
    return n;
}

Detector parse_detector(const std::string& s) {
    if (s == "peak") return Detector::peak;
    if (s == "sample") return Detector::sample;
    if (s == "average") return Detector::average;
    if (s == "negative_peak") return Detector::negative_peak;
    throw std::invalid_argument("unknown detector '" + s + "'");
}

std::vector<double> to_display(const Bins& bins, double start, double stop, std::size_t points, Detector det) {
    std::vector<double> out(points, -300.0);
    if (bins.freq_hz.empty() || points == 0) return out;
    const double step = points > 1 ? (stop - start) / static_cast<double>(points - 1) : (stop - start);
    const auto& f = bins.freq_hz;
    const auto& p = bins.power;
    std::size_t j = 0;
    for (std::size_t i = 0; i < points; ++i) {
        const double fc = start + step * static_cast<double>(i);
        const double lo = fc - step / 2, hi = fc + step / 2;
        while (j < f.size() && f[j] < lo) ++j;
        std::size_t k = j;
        double acc = 0, best = det == Detector::negative_peak ? 1e300 : -1.0;
        std::size_t count = 0, nearest = j;
        double nearest_d = 1e300;
        for (; k < f.size() && f[k] < hi; ++k, ++count) {
            const double v = p[k];
            if (det == Detector::peak) best = std::max(best, v);
            if (det == Detector::negative_peak) best = std::min(best, v);
            acc += v;
            const double d = std::abs(f[k] - fc);
            if (d < nearest_d) {
                nearest_d = d;
                nearest = k;
            }
        }
        double v;
        if (count == 0) {
            // More points than bins: interpolate between the neighbours (in dB).
            if (j == 0) v = p.front();
            else if (j >= f.size()) v = p.back();
            else {
                const double t = (fc - f[j - 1]) / (f[j] - f[j - 1]);
                out[i] = to_db(p[j - 1]) + t * (to_db(p[j]) - to_db(p[j - 1]));
                continue;
            }
        } else if (det == Detector::average) {
            v = acc / static_cast<double>(count);
        } else if (det == Detector::sample) {
            v = p[nearest];
        } else {
            v = best;
        }
        out[i] = to_db(v);
    }
    return out;
}

// --- traces ----------------------------------------------------------------------

const std::vector<std::string>& trace_mode_names() {
    static const std::vector<std::string> n{"clear_write", "max_hold", "min_hold", "average", "view", "blank"};
    return n;
}

TraceMode parse_trace_mode(const std::string& s) {
    if (s == "clear_write") return TraceMode::clear_write;
    if (s == "max_hold") return TraceMode::max_hold;
    if (s == "min_hold") return TraceMode::min_hold;
    if (s == "average") return TraceMode::average;
    if (s == "view") return TraceMode::view;
    if (s == "blank") return TraceMode::blank;
    throw std::invalid_argument("unknown trace mode '" + s + "'");
}

void Trace::set_mode(TraceMode m) {
    if (m == mode_) return;
    mode_ = m;
    if (m != TraceMode::view) reset();  // "view" freezes what is there
}

void Trace::reset() {
    db_.clear();
    linear_.clear();
    count_ = 0;
}

void Trace::update(const std::vector<double>& db, std::size_t average_count) {
    if (mode_ == TraceMode::view || mode_ == TraceMode::blank) return;
    if (db_.size() != db.size()) reset();
    ++count_;
    switch (mode_) {
        case TraceMode::clear_write: db_ = db; break;
        case TraceMode::max_hold:
            if (count_ == 1) db_ = db;
            else for (std::size_t i = 0; i < db.size(); ++i) db_[i] = std::max(db_[i], db[i]);
            break;
        case TraceMode::min_hold:
            if (count_ == 1) db_ = db;
            else for (std::size_t i = 0; i < db.size(); ++i) db_[i] = std::min(db_[i], db[i]);
            break;
        case TraceMode::average: {
            // Running (linear power) average over the last `average_count` sweeps.
            if (count_ == 1) {
                linear_.resize(db.size());
                for (std::size_t i = 0; i < db.size(); ++i) linear_[i] = from_db(db[i]);
            } else {
                const double k = static_cast<double>(std::min<std::size_t>(count_, std::max<std::size_t>(average_count, 1)));
                for (std::size_t i = 0; i < db.size(); ++i) linear_[i] += (from_db(db[i]) - linear_[i]) / k;
            }
            db_.resize(db.size());
            for (std::size_t i = 0; i < db.size(); ++i) db_[i] = to_db(linear_[i]);
            break;
        }
        default: break;
    }
}

// --- peaks -------------------------------------------------------------------------

std::size_t point_index(double freq, double start, double stop, std::size_t points) {
    if (points < 2 || stop <= start) return 0;
    const double t = (freq - start) / (stop - start) * static_cast<double>(points - 1);
    return static_cast<std::size_t>(std::clamp(std::lround(t), 0L, static_cast<long>(points - 1)));
}

std::vector<PeakPoint> find_peaks(const std::vector<double>& db, double start, double stop, double threshold,
                                  double excursion, std::size_t max_peaks) {
    std::vector<PeakPoint> out;
    const std::size_t n = db.size();
    for (std::size_t i = 0; i < n; ++i) {
        const double v = db[i];
        if (v < threshold) continue;
        if (i > 0 && db[i - 1] >= v) continue;      // plateaus count once, at their left edge
        if (i + 1 < n && db[i + 1] > v) continue;
        // Valleys: lowest point on each side before something higher (or the edge).
        double left = v, right = v;
        for (std::size_t k = i; k-- > 0 && db[k] <= v;) left = std::min(left, db[k]);
        for (std::size_t k = i + 1; k < n && db[k] <= v; ++k) right = std::min(right, db[k]);
        const bool edge_left = i == 0, edge_right = i + 1 == n;
        const double valley = edge_left ? right : edge_right ? left : std::max(left, right);
        if (v - valley < excursion && !(edge_left && edge_right)) continue;
        out.push_back({i, point_freq(i, start, stop, n), v});
    }
    std::sort(out.begin(), out.end(), [](const PeakPoint& a, const PeakPoint& b) { return a.db > b.db; });
    if (out.size() > max_peaks) out.resize(max_peaks);
    return out;
}

// --- measurements ------------------------------------------------------------------

double channel_power_db(const Bins& bins, double from, double to, double enbw_bins) {
    // Each bin reads the power in one RBW (= ENBW bins); dividing the sum by
    // the ENBW integrates the spectral density across the channel.
    double sum = 0;
    for (std::size_t i = 0; i < bins.freq_hz.size(); ++i) {
        if (bins.freq_hz[i] >= from && bins.freq_hz[i] <= to) sum += bins.power[i];
    }
    return to_db(sum / std::max(enbw_bins, 1e-9));
}

Occupied occupied_bandwidth(const Bins& bins, double fraction) {
    Occupied o;
    const auto n = bins.power.size();
    if (n < 2) return o;
    double total = 0;
    for (double v : bins.power) total += v;
    if (total <= 0) return o;
    const double lo_target = total * (1 - fraction) / 2;
    const double hi_target = total * (1 + fraction) / 2;
    double acc = 0;
    bool have_lo = false;
    for (std::size_t i = 0; i < n; ++i) {
        acc += bins.power[i];
        if (!have_lo && acc >= lo_target) {
            o.from_hz = bins.freq_hz[i];
            have_lo = true;
        }
        if (acc >= hi_target) {
            o.to_hz = bins.freq_hz[i];
            break;
        }
    }
    o.bandwidth_hz = std::max(0.0, o.to_hz - o.from_hz);
    return o;
}

}  // namespace sa
