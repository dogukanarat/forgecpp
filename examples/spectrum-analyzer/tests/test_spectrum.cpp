// Spectrum analyzer core: planning, DSP scaling, detectors, traces, peaks, measurements.
#include "dsp.hpp"
#include "spectrum.hpp"

#include <forge/testing.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using radio::dsp::Window;

namespace {

std::vector<std::int16_t> tone(std::size_t n, double offset_hz, double rate, double amplitude) {
    std::vector<std::int16_t> iq(2 * n);
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2 * M_PI * offset_hz * static_cast<double>(i) / rate;
        iq[2 * i] = static_cast<std::int16_t>(std::lround(amplitude * 32767 * std::cos(ph)));
        iq[2 * i + 1] = static_cast<std::int16_t>(std::lround(amplitude * 32767 * std::sin(ph)));
    }
    return iq;
}

sa::Bins bins_of(const std::vector<std::int16_t>& iq, std::size_t fft, double rate, double lo, Window w) {
    const radio::dsp::Fft plan(fft);
    const auto win = radio::dsp::make_window(w, fft);
    std::vector<double> spec;
    radio::dsp::power_spectrum(iq.data(), iq.size() / 2 / fft, plan, win, spec);  // FFT count
    sa::Bins b;
    sa::append_bins(b, spec, lo, rate, lo - rate / 2, lo + rate / 2);
    return b;
}

}  // namespace

TEST_CASE("sa: window parameters") {
    CHECK(radio::dsp::make_window(Window::rectangular, 1024).enbw_bins == doctest::Approx(1.0));
    CHECK(radio::dsp::make_window(Window::hann, 1024).enbw_bins == doctest::Approx(1.5).epsilon(0.01));
    CHECK(radio::dsp::make_window(Window::blackman_harris, 1024).enbw_bins == doctest::Approx(2.0).epsilon(0.01));
    CHECK(radio::dsp::make_window(Window::flat_top, 1024).enbw_bins > 3.5);
    CHECK(radio::dsp::parse_window("hann") == Window::hann);
    CHECK_THROWS(radio::dsp::parse_window("triangle"));
}

TEST_CASE("sa: full-scale tone reads 0 dBFS with every window") {
    const double rate = 10e6;
    for (auto w : {Window::rectangular, Window::hann, Window::hamming, Window::blackman_harris, Window::flat_top}) {
        // off-bin tone: only the flat-top window keeps the scalloping loss small
        const double offset = w == Window::flat_top ? 1.2345e6 : 1.25e6;
        const auto b = bins_of(tone(4096, offset, rate, 1.0), 1024, rate, 100e6, w);
        const auto d = sa::to_display(b, 95e6, 105e6, 1001, sa::Detector::peak);
        const auto peak = *std::max_element(d.begin(), d.end());
        CAPTURE(radio::dsp::to_string(w));
        CHECK(peak == doctest::Approx(0.0).epsilon(w == Window::flat_top ? 0.1 : 0.05));
        const auto p = sa::find_peaks(d, 95e6, 105e6, -50, 6, 5);
        REQUIRE(!p.empty());
        CHECK(std::abs(p.front().freq_hz - (100e6 + offset)) < 20e3);
    }
    // -20 dB tone
    const auto b = bins_of(tone(4096, -2e6, rate, 0.1), 1024, rate, 100e6, Window::blackman_harris);
    const auto d = sa::to_display(b, 95e6, 105e6, 1001, sa::Detector::peak);
    CHECK(*std::max_element(d.begin(), d.end()) == doctest::Approx(-20.0).epsilon(0.02));
}

TEST_CASE("sa: channel power integrates the tone regardless of the window") {
    const double rate = 10e6;
    for (auto w : {Window::rectangular, Window::hann, Window::blackman_harris}) {
        const auto win = radio::dsp::make_window(w, 1024);
        const auto b = bins_of(tone(8192, 1.1e6, rate, 0.5), 1024, rate, 100e6, w);
        CHECK(sa::channel_power_db(b, 100.5e6, 101.5e6, win.enbw_bins) == doctest::Approx(-6.02).epsilon(0.03));
        if (w == Window::rectangular) continue;  // off-bin leakage spreads 1 % of the power widely
        const auto obw = sa::occupied_bandwidth(b, 0.99);
        CHECK(obw.bandwidth_hz < 100e3);
        CHECK(std::abs((obw.from_hz + obw.to_hz) / 2 - 101.1e6) < 50e3);
    }
}

TEST_CASE("sa: realtime and swept plans") {
    const auto rt = sa::make_plan(2440e6, 20e6, 0, Window::blackman_harris);
    CHECK_FALSE(rt.swept);
    CHECK(rt.segments.size() == 1);
    CHECK(rt.sample_rate_hz >= 20e6 / 0.8 - 1);
    CHECK(rt.rbw_hz <= 20e6 / 500 * 1.01);
    CHECK(rt.start_hz == 2430e6);
    CHECK(rt.stop_hz == 2450e6);

    // Narrow span: minimum rate, FFT grows for the requested RBW.
    const auto narrow = sa::make_plan(433.92e6, 200e3, 1e3, Window::hann);
    CHECK_FALSE(narrow.swept);
    CHECK(narrow.sample_rate_hz == doctest::Approx(2.083334e6));
    CHECK(narrow.rbw_hz <= 1e3);
    CHECK(radio::dsp::is_power_of_two(narrow.fft_size));

    const auto wide = sa::make_plan(3035e6, 5930e6, 0, Window::blackman_harris);
    CHECK(wide.swept);
    CHECK(wide.segments.size() >= 5930e6 / (61.44e6 * 0.8));
    // segments tile the span without gaps and keep the LO in range
    double edge = wide.start_hz;
    for (const auto& s : wide.segments) {
        CHECK(s.from_hz == doctest::Approx(edge));
        CHECK(s.to_hz > s.from_hz);
        CHECK(s.lo_hz >= 70e6);
        CHECK(s.lo_hz <= 6000e6);
        CHECK(s.from_hz >= s.lo_hz - wide.sample_rate_hz / 2);
        CHECK(s.to_hz <= s.lo_hz + wide.sample_rate_hz / 2);
        edge = s.to_hz;
    }
    CHECK(edge == doctest::Approx(wide.stop_hz));
}

TEST_CASE("sa: detectors") {
    sa::Bins b;
    for (int i = 0; i < 100; ++i) {
        b.freq_hz.push_back(i);
        b.power.push_back(i % 10 == 5 ? 1.0 : 0.01);  // -20 dB floor, 0 dB spikes
    }
    const auto peak = sa::to_display(b, 0, 99, 10, sa::Detector::peak);
    const auto neg = sa::to_display(b, 0, 99, 10, sa::Detector::negative_peak);
    const auto avg = sa::to_display(b, 0, 99, 10, sa::Detector::average);
    for (std::size_t i = 0; i < 10; ++i) {
        CHECK(peak[i] == doctest::Approx(0.0));
        CHECK(neg[i] == doctest::Approx(-20.0));
        CHECK(avg[i] > -20);
        CHECK(avg[i] < 0);
    }
    // More points than bins: values are interpolated, never empty.
    const auto many = sa::to_display(b, 0, 99, 1001, sa::Detector::sample);
    CHECK(many.size() == 1001);
    for (double v : many) CHECK(v >= -20.001);
    CHECK_THROWS(sa::parse_detector("rms2"));
}

TEST_CASE("sa: trace modes") {
    sa::Trace t;
    t.set_mode(sa::TraceMode::max_hold);
    t.update({-10, -50, -30}, 10);
    t.update({-40, -20, -35}, 10);
    CHECK(t.db() == std::vector<double>{-10, -20, -30});
    t.set_mode(sa::TraceMode::min_hold);
    t.update({-10, -50, -30}, 10);
    t.update({-40, -20, -35}, 10);
    CHECK(t.db() == std::vector<double>{-40, -50, -35});
    t.set_mode(sa::TraceMode::view);  // freezes
    t.update({0, 0, 0}, 10);
    CHECK(t.db() == std::vector<double>{-40, -50, -35});
    t.set_mode(sa::TraceMode::average);
    t.update({-10, -10, -10}, 2);
    t.update({-20, -20, -20}, 2);
    CHECK(t.db()[0] < -10);
    CHECK(t.db()[0] > -20);
    t.set_mode(sa::TraceMode::clear_write);
    t.update({1, 2, 3}, 2);
    CHECK(t.db() == std::vector<double>{1, 2, 3});
    t.set_mode(sa::TraceMode::blank);
    CHECK_FALSE(t.visible());
    CHECK(sa::parse_trace_mode("max_hold") == sa::TraceMode::max_hold);
}

TEST_CASE("sa: peak search with threshold and excursion") {
    std::vector<double> db(101, -90.0);
    db[20] = -30;
    db[21] = -33;  // shoulder of the same peak
    db[50] = -60;
    db[80] = -87;  // below the excursion
    const auto p = sa::find_peaks(db, 0, 100, -100, 6, 10);
    REQUIRE(p.size() == 2);
    CHECK(p[0].index == 20);
    CHECK(p[0].freq_hz == 20);
    CHECK(p[1].index == 50);
    CHECK(sa::find_peaks(db, 0, 100, -50, 6, 10).size() == 1);  // threshold
    CHECK(sa::find_peaks(db, 0, 100, -100, 6, 1).size() == 1);  // max peaks
    CHECK(sa::point_index(50, 0, 100, 101) == 50);
    CHECK(sa::point_index(1000, 0, 100, 101) == 100);
}
