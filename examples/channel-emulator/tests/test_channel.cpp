// Every impairment measured on known signals.
#include "channel.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <numeric>
#include <stdexcept>

namespace {

using channel::cf;
constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 1e6;

std::vector<cf> tone(std::size_t n, double hz, float amp = 0.25f, double rate = kRate) {
    std::vector<cf> x(n);
    for (std::size_t i = 0; i < n; ++i) x[i] = std::polar(amp, static_cast<float>(2 * kPi * hz * i / rate));
    return x;
}

/// Mean frequency from the phase steps.
double frequency(const std::vector<cf>& x, std::size_t from, std::size_t to, double rate = kRate) {
    std::complex<double> acc{};
    for (std::size_t i = from + 1; i < to; ++i) acc += std::complex<double>(x[i]) * std::conj(std::complex<double>(x[i - 1]));
    return std::arg(acc) * rate / (2 * kPi);
}

double power(const std::vector<cf>& x, std::size_t from = 0, std::size_t to = 0) {
    if (to == 0) to = x.size();
    double p = 0;
    for (std::size_t i = from; i < to; ++i) p += std::norm(x[i]);
    return p / static_cast<double>(to - from);
}

/// Process in uneven blocks (state must carry over block edges).
void run(channel::Channel& c, std::vector<cf>& x) {
    std::size_t i = 0, k = 0;
    const std::size_t sizes[] = {1000, 4096, 77, 16384, 1};
    while (i < x.size()) {
        const std::size_t n = std::min(sizes[k++ % 5], x.size() - i);
        c.process(x.data() + i, n);
        i += n;
    }
}

}  // namespace

TEST_CASE("channel: bypass and nothing on are transparent; cs16 in place") {
    channel::Channel c(kRate);
    auto x = tone(10000, 12345);
    const auto ref = x;
    run(c, x);
    CHECK(x == ref);
    channel::Settings s = channel::preset("awgn-10");
    s.bypass = true;
    c.configure(s);
    run(c, x);
    CHECK(x == ref);

    std::vector<std::int16_t> iq = {1000, -2000, 32767, -32768, 0, 0};
    const auto iq_ref = iq;
    c.process(iq.data(), 3);
    CHECK(iq == iq_ref);
    c.configure({});
    c.process(iq.data(), 3);
    CHECK(iq == iq_ref);
    channel::Settings loud;
    loud.gain_db = 6;
    c.configure(loud);
    c.process(iq.data(), 3);
    CHECK(c.stats().clipped == 1);  // the sample (32767, -32768): both parts beyond full scale
    CHECK(iq[2] == 32767);
}

TEST_CASE("channel: gain, constant frequency offset, sinusoidal Doppler") {
    channel::Channel c(kRate);
    channel::Settings s;
    s.gain_db = -6;
    s.frequency = true;
    s.cfo_hz = 1234;
    c.configure(s);
    auto x = tone(200000, 20000);
    run(c, x);
    CHECK(power(x) == doctest::Approx(0.0625 * std::pow(10, -0.6)).epsilon(0.001));
    CHECK(frequency(x, 0, x.size()) == doctest::Approx(21234).epsilon(1e-5));

    channel::Channel d(kRate);
    s = {};
    s.frequency = true;
    s.doppler_hz = 5000;
    s.doppler_period_s = 0.4;
    d.configure(s);
    auto y = tone(400000, 0);
    run(d, y);
    // sin: +5000 Hz after a quarter period, -5000 after three quarters
    CHECK(frequency(y, 99000, 101000) == doctest::Approx(5000).epsilon(0.01));
    CHECK(frequency(y, 299000, 301000) == doctest::Approx(-5000).epsilon(0.01));
    CHECK(std::abs(frequency(y, 199500, 200500)) < 50);
}

TEST_CASE("channel: noise at the SNR asked for, in the bandwidth asked for") {
    for (double bw : {0.0, 250e3}) {
        CAPTURE(bw);
        channel::Channel c(kRate, 7);
        channel::Settings s;
        s.noise = true;
        s.snr_db = 20;
        s.noise_bandwidth_hz = bw;
        c.configure(s);
        const auto clean = tone(400000, 1000);
        auto x = clean;
        run(c, x);
        std::vector<cf> n(x.size());
        for (std::size_t i = 0; i < x.size(); ++i) n[i] = x[i] - clean[i];
        const double expect = power(clean) / 100 * (bw > 0 ? kRate / bw : 1);
        CHECK(power(n) == doctest::Approx(expect).epsilon(0.03));
        // white and complex: I and Q alike, uncorrelated from one sample to the next
        double ii = 0, qq = 0, lag = 0;
        for (std::size_t i = 1; i < n.size(); ++i) {
            ii += n[i].real() * n[i].real();
            qq += n[i].imag() * n[i].imag();
            lag += (std::complex<double>(n[i]) * std::conj(std::complex<double>(n[i - 1]))).real();
        }
        CHECK(ii / qq == doctest::Approx(1).epsilon(0.02));
        CHECK(std::abs(lag / (ii + qq)) < 0.01);
    }
}

TEST_CASE("channel: SNR refers to the signal while present (bursts with gaps)") {
    channel::Channel c(kRate, 3);
    channel::Settings s;
    s.noise = true;
    s.snr_db = 10;
    c.configure(s);
    std::vector<cf> x(300000);
    for (std::size_t i = 0; i < x.size(); ++i) {
        if ((i / 5000) % 4 == 0) x[i] = cf(0.1f, 0);  // on 25 % of the time, power 0.01
    }
    run(c, x);
    CHECK(10 * std::log10(c.signal_power()) == doctest::Approx(-20).epsilon(0.01));
    CHECK(power(x, 5001, 19999) == doctest::Approx(0.001).epsilon(0.05));  // a gap: noise only
}

TEST_CASE("channel: multipath impulse response") {
    channel::Channel c(3e6);
    channel::Settings s;
    s.multipath = true;
    s.paths = {{0, 0, 0}, {1.0, -6, 90}};  // 1 us = 3 samples at 3 MSPS
    c.configure(s);
    std::vector<cf> x(64);
    x[0] = cf(1, 0);
    c.process(x.data(), x.size());
    // fixed latency of 8 samples (the fractional-delay filters), power normalized
    const double a0 = 1 / std::sqrt(1 + std::pow(10, -0.6)), a1 = a0 * std::pow(10, -0.3);
    CHECK(std::abs(x[8]) == doctest::Approx(a0).epsilon(0.001));
    CHECK(std::abs(x[11]) == doctest::Approx(a1).epsilon(0.001));
    CHECK(std::arg(x[11]) == doctest::Approx(kPi / 2).epsilon(0.001));
    double rest = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (i != 8 && i != 11) rest += std::norm(x[i]);
    }
    CHECK(rest < 1e-8);

    // a fractional delay spreads over neighbours but keeps the power of a band-limited signal
    s.paths = {{0.5, 0, 0}};  // 1.5 samples
    c.configure(s);
    auto t = tone(20000, 200e3, 0.25f, 3e6);
    c.process(t.data(), t.size());
    CHECK(power(t, 100, t.size()) == doctest::Approx(0.0625).epsilon(0.01));
}

TEST_CASE("channel: Rayleigh and Rician fading statistics") {
    channel::Channel c(kRate, 11);
    channel::Settings s;
    s.fading = true;
    s.fading_hz = 200;
    c.configure(s);
    std::vector<cf> x(4000000, cf(1, 0));
    run(c, x);
    double mean = 0;
    std::size_t deep = 0;
    for (const auto& v : x) {
        mean += std::norm(v);
        deep += std::norm(v) < 0.1;
    }
    mean /= static_cast<double>(x.size());
    const double frac = static_cast<double>(deep) / static_cast<double>(x.size());
    MESSAGE("Rayleigh: mean power " << mean << ", below -10 dB " << frac * 100 << " % (theory 9.5 %)");
    CHECK(mean == doctest::Approx(1).epsilon(0.15));
    CHECK(frac > 0.05);
    CHECK(frac < 0.15);
    // the fading rate follows the Doppler: no faster than fd
    std::size_t crossings = 0;
    for (std::size_t i = 1; i < x.size(); ++i) crossings += (std::norm(x[i]) < 0.1) != (std::norm(x[i - 1]) < 0.1);
    CHECK(crossings / 2.0 / 4.0 < 200);

    s.rician_k_db = 10;
    c.configure(s);
    std::fill(x.begin(), x.end(), cf(1, 0));
    run(c, x);
    deep = 0;
    for (const auto& v : x) deep += std::norm(v) < 0.1;
    CHECK(static_cast<double>(deep) / static_cast<double>(x.size()) < 0.005);
}

TEST_CASE("channel: phase noise is a random walk with the linewidth's variance") {
    channel::Channel c(kRate, 5);
    channel::Settings s;
    s.phase_noise = true;
    s.linewidth_hz = 100;
    c.configure(s);
    std::vector<cf> x(2000000, cf(0.5f, 0));
    run(c, x);
    const std::size_t lag = 1000;
    double var = 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i + lag < x.size(); i += lag) {
        const double d = std::arg(std::complex<double>(x[i + lag]) * std::conj(std::complex<double>(x[i])));
        var += d * d;
        ++n;
    }
    var /= static_cast<double>(n);
    const double expect = 2 * kPi * 100 * lag / kRate;
    MESSAGE("phase variance over 1 ms: " << var << " rad^2 (theory " << expect << ")");
    CHECK(var == doctest::Approx(expect).epsilon(0.2));
    CHECK(power(x) == doctest::Approx(0.25).epsilon(0.001));  // amplitude untouched
}

TEST_CASE("channel: IQ imbalance image and DC offset") {
    channel::Channel c(kRate);
    channel::Settings s;
    s.iq = true;
    s.iq_gain_db = 1;
    s.iq_phase_deg = 3;
    c.configure(s);
    const double f = 100e3;
    auto x = tone(100000, f);
    run(c, x);
    std::complex<double> wanted{}, image{};
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double ph = 2 * kPi * f * i / kRate;
        wanted += std::complex<double>(x[i]) * std::polar(1.0, -ph);
        image += std::complex<double>(x[i]) * std::polar(1.0, ph);
    }
    const double g = std::pow(10, 0.05), p = 3 * kPi / 180;
    const double expect = std::norm((1.0 - g * std::polar(1.0, p)) / (1.0 + g * std::polar(1.0, -p)));
    CHECK(std::norm(image) / std::norm(wanted) == doctest::Approx(expect).epsilon(0.01));
    MESSAGE("image rejection " << -10 * std::log10(expect) << " dB");

    channel::Channel d(kRate);
    s = {};
    s.dc = true;
    s.dc_dbc = -20;
    s.dc_phase_deg = 0;
    d.configure(s);
    auto y = tone(100000, f);
    run(d, y);
    std::complex<double> mean{};
    for (const auto& v : y) mean += std::complex<double>(v);
    mean /= static_cast<double>(y.size());
    CHECK(mean.real() == doctest::Approx(0.25 * 0.1).epsilon(0.01));
    CHECK(std::abs(mean.imag()) < 1e-3);
}

TEST_CASE("channel: interfering tone, continuous and pulsed") {
    channel::Channel c(kRate);
    channel::Settings s;
    s.interference = true;
    s.interference_hz = -300e3;
    s.interference_dbc = -6;
    c.configure(s);
    std::vector<cf> x(100000);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = cf(i % 2 ? 0.2f : -0.2f, 0);  // power 0.04 at fs/2
    std::vector<cf> clean = x;
    run(c, x);
    std::vector<cf> t(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) t[i] = x[i] - clean[i];
    CHECK(power(t) == doctest::Approx(0.04 / std::pow(10, 0.6)).epsilon(0.01));
    CHECK(frequency(t, 0, t.size()) == doctest::Approx(-300e3).epsilon(1e-4));

    s.interference_on_ms = 2;
    s.interference_off_ms = 8;
    c.configure(s);
    x = clean;
    run(c, x);
    std::size_t on = 0;
    for (std::size_t i = 0; i < x.size(); ++i) on += std::abs(x[i] - clean[i]) > 1e-3;
    CHECK(static_cast<double>(on) / static_cast<double>(x.size()) == doctest::Approx(0.2).epsilon(0.05));
}

TEST_CASE("channel: sample clock offset drifts the timing, resets in gaps, keeps the sample count") {
    // a continuous tone read with a clock 200 ppm fast is 200 ppm higher in frequency
    channel::Channel c(kRate);
    channel::Settings s;
    s.clock = true;
    s.clock_ppm = 200;
    c.configure(s);
    auto x = tone(100000, 50e3);
    run(c, x);
    CHECK(x.size() == 100000);
    CHECK(frequency(x, 1000, 100000) == doctest::Approx(50e3 * (1 + 200e-6)).epsilon(2e-6));
    CHECK(power(x, 1000, 100000) == doctest::Approx(0.0625).epsilon(0.002));
    CHECK(c.stats().timing_forced == 0);  // 100 000 * 200e-6 = 20 samples < 32

    // without gaps the delay is forced back at 32 samples; with gaps it resets there
    auto y = tone(300000, 50e3);
    run(c, y);
    CHECK(c.stats().timing_forced >= 1);
    channel::Channel b(kRate);
    b.configure(s);
    auto z = tone(400000, 50e3);
    for (std::size_t i = 0; i < z.size(); ++i) {
        if (i % 20000 >= 15000) z[i] = {};  // 15 ms bursts, 5 ms gaps
    }
    run(b, z);
    CHECK(b.stats().timing_resets >= 18);
    CHECK(b.stats().timing_forced == 0);
}

TEST_CASE("channel: presets") {
    for (const auto& name : channel::preset_names()) {
        CAPTURE(name);
        channel::Channel c(3e6);
        c.configure(channel::preset(name));
        auto x = tone(50000, 100e3, 0.25f, 3e6);
        run(c, x);
        for (const auto& v : x) REQUIRE(std::isfinite(v.real()));
    }
    CHECK(channel::preset("awgn-17").snr_db == 17);
    CHECK_THROWS_AS(channel::preset("nope"), std::invalid_argument);
}
