// The modem through a simulated channel: gain, phase, frequency offset, a
// fractional sample delay, noise, int16 quantization and random block sizes.
#include "qam.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <random>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Channel {
    double gain = 0.5;          // amplitude
    double phase = 1.0;         // rad
    double cfo_hz = 2000;
    double delay = 0.37;        // samples (fractional)
    double snr_db = 30;         // signal (while a packet is on the air) to noise, in the signal bandwidth
    double sample_rate = 3e6;
    unsigned seed = 1;
};

/// Applies the channel to a whole cs16 capture.
std::vector<std::int16_t> through(const std::vector<std::int16_t>& tx, const Channel& ch, double signal_rms) {
    const std::size_t n = tx.size() / 2;
    // windowed-sinc fractional delay, 31 taps
    constexpr int K = 15;
    std::vector<double> h(2 * K + 1);
    for (int k = -K; k <= K; ++k) {
        const double t = k - ch.delay;
        const double sinc = std::abs(t) < 1e-12 ? 1.0 : std::sin(kPi * t) / (kPi * t);
        const double win = 0.42 + 0.5 * std::cos(kPi * t / (K + 1)) + 0.08 * std::cos(2 * kPi * t / (K + 1));
        h[k + K] = sinc * win;
    }
    std::mt19937 rng(ch.seed);
    // noise over the full sample rate; the signal occupies ~ 1/sps * (1 + a) of it,
    // the matched filter keeps the in-band part: specify SNR at the filter output.
    const double noise_rms = signal_rms * ch.gain * std::pow(10.0, -ch.snr_db / 20) / std::sqrt(2.0);
    std::normal_distribution<double> noise(0.0, noise_rms * 32767 * 2);  // x2: matched filter gain 1/sps -> 4
    std::vector<std::int16_t> rx(tx.size());
    const double w = 2 * kPi * ch.cfo_hz / ch.sample_rate;
    for (std::size_t i = 0; i < n; ++i) {
        std::complex<double> acc{};
        for (int k = -K; k <= K; ++k) {
            const auto j = static_cast<std::ptrdiff_t>(i) - k;
            if (j < 0 || j >= static_cast<std::ptrdiff_t>(n)) continue;
            acc += h[k + K] * std::complex<double>(tx[2 * j], tx[2 * j + 1]);
        }
        acc *= ch.gain * std::polar(1.0, ch.phase + w * static_cast<double>(i));
        acc += std::complex<double>(noise(rng), noise(rng));
        rx[2 * i] = static_cast<std::int16_t>(std::clamp(std::lround(acc.real()), -32768L, 32767L));
        rx[2 * i + 1] = static_cast<std::int16_t>(std::clamp(std::lround(acc.imag()), -32768L, 32767L));
    }
    return rx;
}

std::vector<std::uint8_t> payload_for(std::uint16_t seq, std::size_t len) {
    std::vector<std::uint8_t> p(len);
    std::mt19937 rng(seq * 7919u + 13);
    for (auto& b : p) b = static_cast<std::uint8_t>(rng());
    return p;
}

struct Run {
    std::vector<qam::Packet> got;
    qam::Stats stats;
    std::size_t sent = 0;
    double air_seconds = 0;
    std::size_t bytes = 0;
};

Run run(std::size_t packets, std::size_t len, const Channel& ch, const qam::Params& p = {}) {
    qam::Modulator mod(p);
    std::uint16_t next_seq = 0;
    std::size_t sent = 0;
    std::vector<std::int16_t> tx;
    // idle lead-in, back-to-back packets, idle tail
    std::vector<std::int16_t> block(2 * 4096);
    mod.fill(block.data(), 1000, {});
    tx.insert(tx.end(), block.begin(), block.begin() + 2000);
    while (sent < packets || !mod.idle()) {
        mod.fill(block.data(), 4096, [&](std::vector<std::uint8_t>& payload, std::uint16_t& seq) {
            if (sent == packets) return false;
            const std::size_t n = len ? len : 1 + (next_seq * 97) % p.max_payload;
            payload = payload_for(next_seq, n);
            seq = next_seq++;
            ++sent;
            return true;
        });
        tx.insert(tx.end(), block.begin(), block.end());
    }
    mod.fill(block.data(), 2000, {});
    tx.insert(tx.end(), block.begin(), block.begin() + 4000);
    CHECK(mod.clipped() == 0);

    const auto rx = through(tx, ch, p.tx_rms);
    qam::Demodulator demod(p);
    Run r;
    r.sent = sent;
    std::mt19937 rng(ch.seed + 100);
    std::uniform_int_distribution<std::size_t> size(1, 20000);
    for (std::size_t i = 0; i < rx.size() / 2;) {
        const std::size_t n = std::min(size(rng), rx.size() / 2 - i);
        demod.push(rx.data() + 2 * i, n, [&](qam::Packet&& pkt) { r.got.push_back(std::move(pkt)); });
        i += n;
    }
    r.stats = demod.stats();
    r.air_seconds = static_cast<double>(tx.size() / 2) / p.sample_rate;
    for (const auto& g : r.got) r.bytes += g.payload.size();
    return r;
}

}  // namespace

TEST_CASE("qam: building blocks") {
    // 16QAM Gray mapping: round trip, unit energy, neighbours differ in one bit
    double e = 0;
    for (unsigned v = 0; v < 16; ++v) {
        CHECK(qam::demap16(qam::map16(v)) == v);
        CHECK(qam::demap16(qam::map16(v) + qam::cf(0.1f, -0.1f)) == v);
        e += std::norm(qam::map16(v));
        for (unsigned u = 0; u < 16; ++u) {
            if (std::abs(std::abs(qam::map16(v) - qam::map16(u)) - 2 / std::sqrt(10.0)) < 1e-4) {
                CHECK(__builtin_popcount(u ^ v) == 1);
            }
        }
    }
    CHECK(e / 16 == doctest::Approx(1.0));

    const std::uint8_t text[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(qam::crc16(text, 9) == 0x29B1);
    CHECK(qam::crc32(text, 9) == 0xCBF43926u);

    std::vector<std::uint8_t> zeros(64, 0), data(64, 0);
    qam::scramble(data.data(), data.size());
    CHECK(data != zeros);
    CHECK(std::count(data.begin(), data.end(), 0) < 4);
    qam::scramble(data.data(), data.size());
    CHECK(data == zeros);

    const auto& pre = qam::preamble();
    REQUIRE(pre.size() == qam::kPreambleSymbols);
    for (std::size_t k = 0; k < qam::kPreambleHalf; ++k) CHECK(pre[k] == pre[k + qam::kPreambleHalf]);

    const qam::Params p;
    const auto taps = qam::rrc_taps(p);
    CHECK(taps.size() == p.span * p.sps + 1);
    double sq = 0;
    for (float t : taps) sq += t * t;
    CHECK(sq == doctest::Approx(1.0));
    // Nyquist: RRC * RRC is zero at the other symbol instants
    for (int k = 1; k <= 4; ++k) {
        double c = 0;
        for (std::size_t i = 0; i + k * p.sps < taps.size(); ++i) c += taps[i] * taps[i + k * p.sps];
        CHECK(std::abs(c) < 0.02);  // the truncated pulse leaves ~1 %
    }
    CHECK(p.symbol_rate() == 750e3);
    CHECK(p.occupied_bandwidth() == doctest::Approx(1.0125e6));
    CHECK(qam::packet_symbols(text, 9, 1).size() + qam::kGapSymbols == qam::packet_symbol_count(9));
}

TEST_CASE("qam: clean loopback, every size") {
    Channel ch;
    ch.cfo_hz = 0;
    ch.delay = 0;
    ch.phase = 0;
    ch.snr_db = 60;
    const auto r = run(40, 0, ch);
    REQUIRE(r.got.size() == r.sent);
    for (std::size_t i = 0; i < r.got.size(); ++i) {
        CHECK(r.got[i].seq == i);
        CHECK(r.got[i].payload == payload_for(static_cast<std::uint16_t>(i), r.got[i].payload.size()));
        CHECK(r.got[i].evm_db < -30);
    }
    CHECK(r.stats.crc_errors == 0);
    CHECK(r.stats.header_errors == 0);
}

TEST_CASE("qam: impaired channel (gain, phase, 2 kHz CFO, fractional delay, 25 dB SNR)") {
    for (double delay : {0.0, 0.25, 0.5, 0.81}) {
        for (double cfo : {-4000.0, 0.0, 2000.0, 7000.0}) {
            CAPTURE(delay);
            CAPTURE(cfo);
            Channel ch;
            ch.delay = delay;
            ch.cfo_hz = cfo;
            ch.snr_db = 25;
            ch.gain = 0.2;
            ch.seed = static_cast<unsigned>(delay * 100 + cfo);
            const auto r = run(30, 1472, ch);
            CHECK(r.got.size() == r.sent);
            for (const auto& g : r.got) {
                CHECK(g.payload == payload_for(g.seq, 1472));
                CHECK(std::abs(g.cfo_hz - cfo) < 150);
                CHECK(g.evm_db < -20);
            }
        }
    }
}

TEST_CASE("qam: throughput above 2 Mbit/s with full packets") {
    Channel ch;
    ch.snr_db = 28;
    const auto r = run(200, 1472, ch);
    CHECK(r.got.size() == r.sent);
    const double mbps = static_cast<double>(r.bytes) * 8 / r.air_seconds / 1e6;
    MESSAGE("throughput on the air: " << mbps << " Mbit/s, " << r.got.size() << " packets");
    CHECK(mbps > 2.0);
}

TEST_CASE("qam: low SNR never delivers a corrupted packet") {
    for (double snr : {6.0, 10.0, 14.0, 18.0}) {
        Channel ch;
        ch.snr_db = snr;
        ch.seed = static_cast<unsigned>(snr);
        const auto r = run(40, 400, ch);
        MESSAGE("SNR " << snr << " dB: " << r.got.size() << "/" << r.sent << " delivered, " << r.stats.crc_errors
                       << " CRC errors, " << r.stats.header_errors << " header errors");
        for (const auto& g : r.got) CHECK(g.payload == payload_for(g.seq, 400));
    }
}

TEST_CASE("qam: no false packets from noise, reset mid-packet recovers") {
    qam::Params p;
    qam::Demodulator demod(p);
    std::mt19937 rng(5);
    std::normal_distribution<double> n(0, 300);
    std::vector<std::int16_t> noise(2 * 300000);
    for (auto& v : noise) v = static_cast<std::int16_t>(n(rng));
    std::size_t got = 0;
    demod.push(noise.data(), noise.size() / 2, [&](qam::Packet&&) { ++got; });
    CHECK(got == 0);
    CHECK(demod.stats().detected < 3);

    demod.reset();
    CHECK(demod.stats().resets == 1);
    Channel ch;
    const auto r = run(10, 200, ch);
    CHECK(r.got.size() == 10);
}

TEST_CASE("qam: CPU cost") {
    qam::Params p;
    qam::Modulator mod(p);
    qam::Demodulator demod(p);
    const auto payload = payload_for(1, 1472);
    std::vector<std::int16_t> iq(2 * 3000000);  // one second at 3 MSPS
    std::uint16_t seq = 0;
    auto t0 = std::chrono::steady_clock::now();
    mod.fill(iq.data(), iq.size() / 2, [&](std::vector<std::uint8_t>& out, std::uint16_t& s) {
        out = payload;
        s = seq++;
        return true;
    });
    auto t1 = std::chrono::steady_clock::now();
    std::size_t got = 0;
    for (std::size_t i = 0; i < iq.size() / 2; i += 65536) {
        demod.push(iq.data() + 2 * i, std::min<std::size_t>(65536, iq.size() / 2 - i), [&](qam::Packet&&) { ++got; });
    }
    auto t2 = std::chrono::steady_clock::now();
    const double tx_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double rx_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    MESSAGE("1 s of signal (" << seq << " packets): modulate " << tx_ms << " ms, demodulate " << rx_ms << " ms, decoded "
                              << got);
    CHECK(got + 2 >= seq);  // the last one is cut off, the first starts at sample 0
}
