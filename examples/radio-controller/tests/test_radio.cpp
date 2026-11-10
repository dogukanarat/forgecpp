#include "backend.hpp"
#include "dsp.hpp"
#include "radio/radio.forge.hpp"
#include "radio_controller.hpp"

#include <forge/remote.hpp>
#include <forge/testing.hpp>
#include <forge/zmq.hpp>

#include <cmath>
#include <complex>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using forge::json;
namespace fz = forge::zmq;
using forge::pb::view;

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

radio_iq_block block_of(const fz::Message& m) { return m.as<radio_iq_block>(); }

std::vector<std::int16_t> samples_of(const fz::Message& m) {
    std::vector<std::int16_t> iq(m.blob.size() / 2);
    std::memcpy(iq.data(), m.blob.data(), m.blob.size());
    return iq;
}

/// Send a TX block the way clients do: [tx][stream_header][radio.tx_block][cs16].
bool send_tx(fz::Sender& push, const std::vector<std::int16_t>& iq, bool cyclic) {
    radio_tx_block b{};
    std::strcpy(b.format, "cs16");
    b.cyclic = cyclic;
    return push.publish("tx", b, iq.data(), iq.size() * 2);
}

/// The HTTP-like status of a refused command; 200 when it went through.
template <class F>
int status_of(F&& f) {
    try {
        f();
        return 200;
    } catch (const forge::Error& e) {
        return e.status();
    }
}

double peak_offset(const std::vector<std::int16_t>& iq, std::size_t n, double rate) {
    const auto spec = radio::dsp::spectrum_dbfs(iq.data(), n);
    return radio::dsp::find_peak(spec, rate).offset_hz;
}

}  // namespace

TEST_CASE("radio: AD936x sample format conversion") {
    using radio::from_full_scale;
    using radio::to_full_scale;
    // RX: 12 significant bits, LSB aligned (le:S12/16>>0)
    CHECK(to_full_scale(2047, 12, 0) == 32752);
    CHECK(to_full_scale(-2048, 12, 0) == -32768);
    CHECK(to_full_scale(-1, 12, 0) == -16);
    // TX: MSB aligned (12 bits stored 4 bits up)
    CHECK(from_full_scale(32767, 12, 4) == 32752);
    CHECK(from_full_scale(-32768, 12, 4) == -32768);
    // LSB-aligned TX variant
    CHECK(from_full_scale(32767, 12, 0) == 2047);
    CHECK(from_full_scale(-32768, 12, 0) == -2048);
    for (int v : {-2048, -1000, -1, 0, 1, 1234, 2047}) {
        const auto word = static_cast<std::int16_t>(v);
        CHECK(from_full_scale(to_full_scale(word, 12, 0), 12, 0) == word);
    }
}

TEST_CASE("radio: FFT, spectrum and power") {
    const double rate = 1e6;
    const auto iq = tone(1024, 125e3, rate, 1.0);
    CHECK(std::abs(peak_offset(iq, 1024, rate) - 125e3) <= rate / 1024);
    CHECK(radio::dsp::power_dbfs(iq.data(), 1024) == doctest::Approx(0.0).epsilon(0.01));
    const auto spec = radio::dsp::spectrum_dbfs(iq.data(), 1024);
    CHECK(radio::dsp::find_peak(spec, rate).dbfs == doctest::Approx(0.0).epsilon(0.1));

    const auto neg = tone(1024, -250e3, rate, 0.1);
    CHECK(std::abs(peak_offset(neg, 1024, rate) + 250e3) <= rate / 1024);
    CHECK(radio::dsp::power_dbfs(neg.data(), 1024) == doctest::Approx(-20.0).epsilon(0.01));
}

TEST_CASE("radio: simulator shows carriers at their offset from the LO") {
    auto b = radio::make_sim_backend();
    radio::Settings s;
    s.rx_lo_hz = 2410e6;  // carrier at 2412 MHz -> +2 MHz
    s.sample_rate_hz = 10e6;
    s.rx_bandwidth_hz = 10e6;
    s.gain_mode = "manual";
    s.rx_gain_db = 30;
    b->apply(s);
    CHECK(b->rx_gain() == 30);

    std::vector<std::int16_t> iq(2 * 4096);
    b->start_rx(4096);
    REQUIRE(b->read_rx(iq.data(), 4096) == 4096);
    CHECK(std::abs(peak_offset(iq, 4096, s.sample_rate_hz) - 2e6) < 5e3);

    // Retune: the carrier moves; out of band it disappears.
    b->set_rx_lo(2413e6);
    REQUIRE(b->read_rx(iq.data(), 4096) == 4096);
    CHECK(std::abs(peak_offset(iq, 4096, s.sample_rate_hz) + 1e6) < 5e3);

    // AGC owns the gain outside manual mode.
    b->set_gain_mode("slow_attack");
    CHECK_THROWS_AS(b->set_rx_gain(10), radio::DeviceError);
    b->cancel_rx();
    CHECK(b->read_rx(iq.data(), 4096) == 0);
}

TEST_CASE("radio: simulator TX loopback (cyclic tone)") {
    auto b = radio::make_sim_backend();
    radio::Settings s;
    s.rx_lo_hz = s.tx_lo_hz = 2400e6;  // no simulated carriers within +-5 MHz
    s.sample_rate_hz = 10e6;
    s.rx_bandwidth_hz = 10e6;
    s.gain_mode = "manual";
    s.rx_gain_db = 20;
    s.tx_attenuation_db = 0;
    b->apply(s);

    const auto tx = tone(1000, 1.5e6, s.sample_rate_hz, 0.5);
    b->start_tx(1000, true);
    b->write_tx(tx.data(), 1000);

    std::vector<std::int16_t> iq(2 * 4096);
    b->start_rx(4096);
    REQUIRE(b->read_rx(iq.data(), 4096) == 4096);
    CHECK(std::abs(peak_offset(iq, 4096, s.sample_rate_hz) - 1.5e6) < 5e3);

    // TX LO 0.5 MHz higher -> the tone shows up 0.5 MHz higher in RX.
    b->set_tx_lo(2400.5e6);
    REQUIRE(b->read_rx(iq.data(), 4096) == 4096);
    CHECK(std::abs(peak_offset(iq, 4096, s.sample_rate_hz) - 2.0e6) < 5e3);

    b->stop_tx();
    REQUIRE(b->read_rx(iq.data(), 4096) == 4096);
    CHECK(radio::dsp::power_dbfs(iq.data(), 4096) < -60);  // only noise left
}

TEST_CASE("radio: simulated AD9361 streams RX1 and RX2 with per-channel gain") {
    radio::SimOptions o;
    o.channels = 2;
    o.loopback_tx = 0;
    o.loopback_rx = 1;  // TX1 -> RX2 cable
    auto b = radio::make_sim_backend(o);
    REQUIRE(b->rx_channel_count() == 2);
    radio::Settings s;
    s.rx_lo_hz = 2410e6;  // 2412 MHz carrier at +2 MHz
    s.tx_lo_hz = 2410e6;
    s.sample_rate_hz = 10e6;
    s.rx_bandwidth_hz = 10e6;
    s.gain_mode = "manual";
    s.rx_gain_db = 40;
    s.rx2_gain_mode = "manual";
    s.rx2_gain_db = 20;
    b->apply(s);
    CHECK(b->rx_gain(0) == 40);
    CHECK(b->rx_gain(1) == 20);

    // cyclic test tone at TX LO + 1 MHz, coupled into RX2 only
    b->set_tx_attenuation(0);
    const auto tx = tone(1000, 1e6, 10e6, 0.5);
    b->start_tx(1000, true);
    b->write_tx(tx.data(), 1000);

    const std::size_t n = 8192;
    std::vector<std::int16_t> rx1(2 * n), rx2(2 * n);
    b->start_rx(n, radio::Backend::kRx1 | radio::Backend::kRx2);
    REQUIRE(b->read_rx(rx1.data(), rx2.data(), n) == n);
    REQUIRE(b->read_rx(rx1.data(), rx2.data(), n) == n);
    const auto s1 = radio::dsp::spectrum_dbfs(rx1.data(), n);
    const auto s2 = radio::dsp::spectrum_dbfs(rx2.data(), n);
    auto level_at = [&](const std::vector<float>& sp, double offset) {
        const auto i = static_cast<std::size_t>(std::lround(offset / 10e6 * n + n / 2.0));
        return *std::max_element(sp.begin() + static_cast<std::ptrdiff_t>(i) - 3,
                                 sp.begin() + static_cast<std::ptrdiff_t>(i) + 4);
    };
    // same air on both channels, 20 dB apart (gain)
    CHECK(level_at(s1, 2e6) - level_at(s2, 2e6) == doctest::Approx(20).epsilon(0.05));
    // the loopback tone is on RX2 only
    CHECK(level_at(s2, 1e6) > -40);
    CHECK(level_at(s1, 1e6) < -80);
    // RX1 only: RX2 pointer is ignored
    b->start_rx(n, radio::Backend::kRx1);
    CHECK(b->read_rx(rx1.data(), n) == n);
    // RX2 only
    b->start_rx(n, radio::Backend::kRx2);
    CHECK(b->read_rx(nullptr, rx2.data(), n) == n);

    // TX2 is not cabled: nothing new on RX2
    b->stop_tx();
    b->set_tx_attenuation(0, 1);
    b->start_tx(1000, true, 1);
    b->write_tx(tx.data(), 1000);
    b->start_rx(n, radio::Backend::kRx2);
    REQUIRE(b->read_rx(nullptr, rx2.data(), n) == n);
    CHECK(level_at(radio::dsp::spectrum_dbfs(rx2.data(), n), 1e6) < -80);
}

TEST_CASE("radio: single-RX parts reject RX2") {
    auto b = radio::make_sim_backend();  // AD9364
    CHECK(b->rx_channel_count() == 1);
    CHECK(b->tx_channel_count() == 1);
    CHECK_THROWS_AS(b->start_rx(4096, radio::Backend::kRx2), radio::DeviceError);
    CHECK_THROWS_AS(b->set_gain_mode("manual", 1), radio::DeviceError);
    CHECK_THROWS_AS(b->rx_gain(1), radio::DeviceError);
    CHECK_THROWS_AS(b->start_tx(1024, false, 1), radio::DeviceError);
    CHECK_THROWS_AS(b->set_tx_attenuation(10, 1), radio::DeviceError);
    radio::Settings s;
    s.rx2_gain_mode = "manual";
    b->apply(s);  // RX2 settings are skipped, not an error
}

TEST_CASE("radio: settings from JSON are clamped to AD9364 limits") {
    const auto s = radio::settings_from_json(
        {{"rx_lo_hz", 10e9}, {"sample_rate_hz", 1e3}, {"gain_mode", "bogus"}, {"tx_attenuation_db", 5}});
    CHECK(s.rx_lo_hz == radio::Limits::lo_max_hz);
    CHECK(s.sample_rate_hz == radio::Limits::rate_min_hz);
    CHECK(s.gain_mode == "slow_attack");
    CHECK(s.tx_attenuation_db == 5);
    CHECK(radio::to_json(s)["rx_lo_hz"] == radio::Limits::lo_max_hz);
}

TEST_CASE("radio-controller: IQ over ZeroMQ, control commands and TX loopback") {
    forge::testing::TempDir dir;
    const auto state = (dir / "state.json").string();
    std::vector<std::string> args{"radio-controller",
                                  "-c",
                                  std::string(RADIO_TEST_DATA_DIR) + "/empty.toml",
                                  "--backend",
                                  "sim",
                                  "-D",
                                  "log.console=false",
                                  "-D",
                                  "http.enabled=false",
                                  "-D",
                                  "forge.data=tcp://127.0.0.1:*",
                                  "-D",
                                  "zmq.tx=tcp://127.0.0.1:*",
                                  "-D",
                                  "forge.control=tcp://127.0.0.1:*",
                                  "-D",
                                  "radio.rx_lo_hz=2410e6",
                                  "-D",
                                  "radio.tx_lo_hz=2410e6",
                                  "-D",
                                  "radio.sample_rate_hz=5e6",
                                  "-D",
                                  "radio.rx_bandwidth_hz=5e6",
                                  "-D",
                                  "radio.buffer_size=16384",
                                  "-D",
                                  "radio.state_file=" + state};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());

    radio::RadioController app;
    std::promise<void> started;
    app.hook_start([&](forge::Application& a) { a.post([&] { started.set_value(); }); });
    std::thread runner([&] { CHECK(app.run(static_cast<int>(argv.size()), argv.data()) == 0); });
    started.get_future().get();

    // --- RX: subscribe to the IQ stream ---------------------------------------
    std::mutex m;
    std::vector<fz::Message> frames;
    fz::Receiver::Options opts;
    opts.topics = {"iq"};
    fz::Receiver sub(fz::Receiver::Kind::sub, app.data_endpoint(), [&](fz::Message&& msg) {
        std::lock_guard lock(m);
        if (frames.size() < 64) frames.push_back(std::move(msg));
    }, opts);
    REQUIRE(forge::testing::wait_until([&] {
        std::lock_guard lock(m);
        return frames.size() >= 3;
    }, 10s));
    {
        std::lock_guard lock(m);
        const auto h = block_of(frames.back());
        CHECK(view(frames.back().header.type) == "radio.iq_block");
        CHECK(view(frames.back().header.source) == "radio-controller");
        CHECK(view(h.format) == "cs16");
        CHECK(h.samples == 16384);
        CHECK(h.sample_rate == 5e6);
        CHECK(h.center_freq == 2410e6);
        CHECK(frames.back().blob.size() == 16384 * 4);
        CHECK(frames[1].header.seq > frames[0].header.seq);
    }

    // --- control over ZeroMQ ---------------------------------------------------
    forge::Remote ctl(app.control_endpoint(), 5s);
    CHECK(ctl.get("connected") == true);
    CHECK(ctl.set("rx_lo_mhz", 2411.5) == 2411.5);
    CHECK(status_of([&] { ctl.set("rx_gain_db", 20); }) == 409);  // AGC owns the gain
    CHECK(status_of([&] { ctl.set("rx_lo_mhz", 9000); }) == 400);  // above 6 GHz
    ctl.set("gain_mode", "manual");
    CHECK(ctl.set("rx_gain_db", 20) == 20);

    auto latest = [&]() {
        std::lock_guard lock(m);
        frames.clear();
        return 0;
    };
    latest();
    REQUIRE(forge::testing::wait_until([&] {
        std::lock_guard lock(m);
        return !frames.empty() && block_of(frames.back()).center_freq == 2411.5e6;
    }, 10s));

    // --- TX: disabled drops, enabled loops back into RX --------------------------
    fz::Sender push(fz::Sender::Kind::push, app.tx_endpoint(), false);
    const auto tx = tone(4000, 1.25e6, 5e6, 0.5);
    REQUIRE(forge::testing::wait_until([&] { return send_tx(push, tx, false); }));
    CHECK(forge::testing::wait_until([&] { return ctl.get("tx_dropped") == 1; }));

    ctl.set("tx_enabled", true);
    ctl.set("tx_lo_mhz", 2411.5);
    ctl.set("tx_atten_db", 0);
    REQUIRE(send_tx(push, tx, true));
    CHECK(forge::testing::wait_until([&] { return ctl.get("tx_mode") == "cyclic"; }));

    latest();
    REQUIRE(forge::testing::wait_until([&] {
        std::lock_guard lock(m);
        if (frames.empty()) return false;
        return std::abs(peak_offset(samples_of(frames.back()), 8192, 5e6) - 1.25e6) < 5e3;
    }, 10s));

    ctl.press("tx_stop");
    CHECK(ctl.get("tx_mode") == "idle");

    app.stop();
    runner.join();

    // Runtime changes were persisted for the next start.
    const auto saved = forge::load_json(state);
    CHECK(saved["rx_lo_hz"] == 2411.5e6);
    CHECK(saved["gain_mode"] == "manual");
    CHECK(saved["tx_enabled"] == true);
}

namespace {
struct RunningRadio {
    explicit RunningRadio(std::vector<std::string> extra) {
        args = {"radio-controller",
                "-c",
                std::string(RADIO_TEST_DATA_DIR) + "/empty.toml",
                "--backend",
                "sim",
                "-D",
                "log.console=false",
                "-D",
                "http.enabled=false",
                "-D",
                "forge.data=tcp://127.0.0.1:*",
                "-D",
                "zmq.tx=tcp://127.0.0.1:*",
                "-D",
                "forge.control=tcp://127.0.0.1:*"};
        args.insert(args.end(), extra.begin(), extra.end());
        for (auto& a : args) argv.push_back(a.data());
        std::promise<void> started;
        app.hook_start([&](forge::Application& a) { a.post([&] { started.set_value(); }); });
        runner = std::thread([this] { CHECK(app.run(static_cast<int>(argv.size()), argv.data()) == 0); });
        started.get_future().get();
    }
    ~RunningRadio() {
        app.stop();
        runner.join();
    }
    std::vector<std::string> args;
    std::vector<char*> argv;
    radio::RadioController app;
    std::thread runner;
};
}  // namespace

TEST_CASE("radio-controller: channel roles from the configuration (AD9361: RX1 own, RX2 shared, TX1 -> RX2 cable)") {
    RunningRadio r(
        {"-D", "radio.sim.channels=2",    "-D", "radio.sim.loopback_tx=1",  "-D", "radio.sim.loopback_rx=2",
         "-D", "radio.rx_channels=[1]",   "-D", "radio.tx_channel=1",       "-D", "radio.tap_rx=[2]",
         "-D", "radio.tap_samples=16384", "-D", "radio.tap_hz=50",          "-D", "radio.rx_lo_hz=2410e6",
         "-D", "radio.tx_lo_hz=2410e6",   "-D", "radio.sample_rate_hz=5e6", "-D", "radio.rx_bandwidth_hz=5e6",
         "-D", "radio.buffer_size=32768"});
    std::mutex m;
    std::vector<fz::Message> tap, rx1;
    fz::Receiver::Options tap_opts;
    tap_opts.topics = {"rx2"};
    fz::Receiver sub2(fz::Receiver::Kind::sub, r.app.data_endpoint(), [&](fz::Message&& msg) {
        std::lock_guard lock(m);
        if (tap.size() < 400) tap.push_back(std::move(msg));
    }, tap_opts);
    fz::Receiver::Options opts;
    opts.topics = {"iq"};
    fz::Receiver sub1(fz::Receiver::Kind::sub, r.app.data_endpoint(), [&](fz::Message&& msg) {
        std::lock_guard lock(m);
        if (rx1.size() < 400) rx1.push_back(std::move(msg));
    }, opts);
    REQUIRE(forge::testing::wait_until([&] {
        std::lock_guard lock(m);
        return tap.size() >= 2 && !rx1.empty();
    }, 10s));
    {
        std::lock_guard lock(m);
        const auto h = block_of(tap.back());
        CHECK(tap.back().topic == "rx2");
        CHECK(h.channel == 2);
        CHECK(h.samples == 16384);  // the start of a 32768-sample DMA buffer
        CHECK(view(h.format) == "cs16");
        CHECK(h.center_freq == 2410e6);
        CHECK(h.sample_rate == 5e6);
        CHECK(tap.back().blob.size() == 16384 * 4);
        CHECK(tap[1].header.seq > tap[0].header.seq);
        for (const auto& f : rx1) CHECK(block_of(f).channel == 1);  // it listens to RX1 only
    }

    forge::Remote ctl(r.app.control_endpoint(), 5s);
    CHECK(ctl.get("hw_channels") == "2R2T");
    CHECK(ctl.get("rx_channels") == json::array({1}));
    CHECK(ctl.get("tap_channels") == json::array({2}));
    CHECK(ctl.get("tx_channel") == "TX1");
    ctl.set("rx2_gain_mode", "manual");
    CHECK(ctl.set("rx2_gain_db", 25) == 25);
    auto newest = [&](std::vector<fz::Message>& v) {
        std::lock_guard lock(m);
        return v.empty() ? fz::Message{} : v.back();
    };
    auto gain_of = [](const fz::Message& msg) { return msg.body.empty() ? 0.0 : block_of(msg).rx_gain_db; };
    auto center_of = [](const fz::Message& msg) { return msg.body.empty() ? 0.0 : block_of(msg).center_freq; };
    auto clear = [&] {
        std::lock_guard lock(m);
        rx1.clear();
        tap.clear();
    };
    clear();
    REQUIRE(forge::testing::wait_until([&] { return gain_of(newest(tap)) == 25.0; }));

    // A cyclic tone on TX1 shows up on RX2 (cable) but not on RX1.
    ctl.set("tx_enabled", true);
    ctl.set("tx_atten_db", 0);
    fz::Sender push(fz::Sender::Kind::push, r.app.tx_endpoint(), false);
    const auto tx = tone(4000, 1.25e6, 5e6, 0.5);
    REQUIRE(forge::testing::wait_until([&] { return send_tx(push, tx, true); }));
    auto peak_of = [&](const fz::Message& msg, std::size_t n) { return peak_offset(samples_of(msg), n, 5e6); };
    clear();
    CHECK(forge::testing::wait_until([&] {
        const auto b = newest(tap);
        return !b.blob.empty() && std::abs(peak_of(b, 16384) - 1.25e6) < 5e3;
    }, 10s));
    clear();
    REQUIRE(forge::testing::wait_until([&] { return !newest(rx1).blob.empty(); }));
    CHECK(std::abs(peak_of(newest(rx1), 8192) - 2e6) < 5e3);  // RX1: the strongest is the 2412 MHz carrier

    // Retuning (radio-controller owns the shared RX LO) is visible in the headers.
    ctl.set("rx_lo_mhz", 2411);
    CHECK(forge::testing::wait_until([&] { return center_of(newest(tap)) == 2411e6; }));
}

TEST_CASE("radio-controller: listens to both RX channels and drives TX2") {
    RunningRadio r(
        {"-D", "radio.sim.channels=2",     "-D", "radio.sim.loopback_tx=2",  "-D", "radio.sim.loopback_rx=1",
         "-D", "radio.rx_channels=[1, 2]", "-D", "radio.tx_channel=2",       "-D", "radio.rx_lo_hz=2410e6",
         "-D", "radio.tx_lo_hz=2410e6",    "-D", "radio.sample_rate_hz=5e6", "-D", "radio.rx_bandwidth_hz=5e6",
         "-D", "radio.buffer_size=8192",   "-D", "radio.tx_enabled=true",    "-D", "radio.tx_attenuation_db=0"});
    std::mutex m;
    std::map<int, fz::Message> last;
    fz::Receiver::Options opts;
    opts.topics = {"iq"};
    fz::Receiver sub(fz::Receiver::Kind::sub, r.app.data_endpoint(), [&](fz::Message&& msg) {
        std::lock_guard lock(m);
        last[static_cast<int>(block_of(msg).channel)] = std::move(msg);
    }, opts);
    fz::Sender push(fz::Sender::Kind::push, r.app.tx_endpoint(), false);
    const auto tx = tone(4000, 1.25e6, 5e6, 0.5);
    REQUIRE(forge::testing::wait_until([&] { return send_tx(push, tx, true); }));
    auto peak = [&](int ch) {
        std::lock_guard lock(m);
        if (!last.count(ch)) return 0.0;
        return peak_offset(samples_of(last[ch]), 8192, 5e6);
    };
    // TX2 is cabled to RX1 in this setup: the tone is on RX1, RX2 sees the carrier
    CHECK(forge::testing::wait_until([&] { return std::abs(peak(1) - 1.25e6) < 5e3; }, 10s));
    // RX2's block of the same buffer may still be on its way
    CHECK(forge::testing::wait_until([&] { return std::abs(peak(2) - 2e6) < 5e3; }, 10s));
    forge::Remote ctl(r.app.control_endpoint(), 5s);
    CHECK(ctl.get("rx_channels") == json::array({1, 2}));
    CHECK(ctl.get("tx_channel") == "TX2");
}

TEST_CASE("radio-controller: streaming TX blocks older than tx_max_age_ms are dropped, not played late") {
    RunningRadio r({"-D", "radio.tx_enabled=true", "-D", "radio.sample_rate_hz=5e6", "-D", "radio.buffer_size=8192"});
    forge::Remote ctl(r.app.control_endpoint(), 5s);
    CHECK(ctl.get("tx_max_age_ms") == 0.0);  // off by default
    ctl.set("tx_max_age_ms", 100);
    fz::Sender push(fz::Sender::Kind::push, r.app.tx_endpoint(), false);
    const auto iq = tone(4096, 1e5, 5e6, 0.3);
    auto block = [&](std::int64_t age_ms) {  // a sender whose block waited age_ms somewhere
        radio_tx_block b{};
        std::strcpy(b.format, "cs16");
        auto m = fz::make_message("tx", b, std::string(reinterpret_cast<const char*>(iq.data()), iq.size() * 2));
        m.header.time_unix_ns -= age_ms * 1000000;
        return m;
    };
    REQUIRE(forge::testing::wait_until([&] { return push.send(block(0)); }));
    CHECK(forge::testing::wait_until([&] { return ctl.get("tx_frames") == 1.0; }));
    REQUIRE(push.send(block(500)));
    REQUIRE(push.send(block(500)));
    CHECK(forge::testing::wait_until([&] { return ctl.get("tx_late") == 2.0; }));
    CHECK(ctl.get("tx_frames") == 1.0);
    REQUIRE(push.send(block(10)));
    CHECK(forge::testing::wait_until([&] { return ctl.get("tx_frames") == 2.0; }));
    ctl.set("tx_max_age_ms", 0);  // off: old blocks play
    REQUIRE(push.send(block(500)));
    CHECK(forge::testing::wait_until([&] { return ctl.get("tx_frames") == 3.0; }));
    CHECK(ctl.get("tx_late") == 2.0);
}

TEST_CASE("radio-controller: a configuration the transceiver cannot do is reported") {
    RunningRadio r({"-D", "radio.tap_rx=[2]"});  // simulated AD9364: one RX, one TX
    forge::Remote ctl(r.app.control_endpoint(), 5s);
    CHECK(ctl.get("connected") == false);
    const auto alarms = r.app.alarms().list();
    REQUIRE(alarms.size() == 1);
    CHECK(alarms[0].id == "radio.device");
    const auto msg = alarms[0].message;
    CHECK(msg.find("RX1, RX2") != std::string::npos);
    CHECK(msg.find("1 RX / 1 TX") != std::string::npos);
    CHECK(msg.find("tap_rx") != std::string::npos);
}

#ifdef RADIO_HAS_IIO
// libiio's XML backend builds a real IIO context from tests/data/ad9364.xml.
// It is read-only, which lets us check device discovery and error handling.

TEST_CASE("radio: libiio backend finds the AD936x devices and reports device errors") {
    const std::string uri = std::string("xml:") + RADIO_TEST_DATA_DIR + "/ad9364.xml";
    REQUIRE(radio::iio_supported());
    auto b = radio::make_iio_backend(uri, 1000);
    CHECK(b->name() == "iio:" + uri);
    const auto info = b->describe();
    CHECK(info["attrs"]["hw_model"] == "AD9364 test fixture");
    CHECK(info["libiio"].get<std::string>().rfind("0.", 0) == 0);

    try {
        b->set_rx_lo(2400e6);
        FAIL("expected DeviceError");
    } catch (const radio::DeviceError& e) {
        CHECK(std::string(e.what()).find("cannot set RX LO to 2400000000") != std::string::npos);
    }
    CHECK_THROWS_AS(b->rx_gain(), radio::DeviceError);
    CHECK_FALSE(b->temperature_c().has_value());          // unreadable -> no value, not an error
    CHECK_THROWS_AS(b->set_rx_gain(10), radio::DeviceError);  // not in manual mode
    CHECK_THROWS_AS(b->start_rx(4096), radio::DeviceError);   // XML backend has no buffers
}

TEST_CASE("radio: libiio backend detects RX2 on an AD9361 and its absence on an AD9364") {
    auto two = radio::make_iio_backend(std::string("xml:") + RADIO_TEST_DATA_DIR + "/ad9361.xml", 1000);
    CHECK(two->rx_channel_count() == 2);
    CHECK(two->describe()["rx_channels"] == 2);
    CHECK(two->tx_channel_count() == 1);  // the fixture has no TX2 channels
    CHECK_THROWS_AS(two->rx_gain(1), radio::DeviceError);  // channel exists, XML attributes are unreadable

    auto one = radio::make_iio_backend(std::string("xml:") + RADIO_TEST_DATA_DIR + "/ad9364.xml", 1000);
    CHECK(one->rx_channel_count() == 1);
    try {
        one->start_rx(4096, radio::Backend::kRx2);
        FAIL("expected DeviceError");
    } catch (const radio::DeviceError& e) {
        CHECK(std::string(e.what()).find("one RX channel (AD9364") != std::string::npos);
    }
    try {
        one->set_gain_mode("manual", 1);
        FAIL("expected DeviceError");
    } catch (const radio::DeviceError& e) {
        CHECK(std::string(e.what()).find("RX2 needs an AD9361/AD9363") != std::string::npos);
    }
}

TEST_CASE("radio: libiio backend explains what it found when the AD936x is missing") {
    const std::string uri = std::string("xml:") + RADIO_TEST_DATA_DIR + "/no_rx.xml";
    try {
        radio::make_iio_backend(uri, 1000);
        FAIL("expected DeviceError");
    } catch (const radio::DeviceError& e) {
        const std::string what = e.what();
        CHECK(what.find("no AD936x found") != std::string::npos);
        CHECK(what.find("some-other-adc") != std::string::npos);
    }
    CHECK_THROWS_AS(radio::make_iio_backend("ip:192.0.2.1", 300), radio::DeviceError);  // unreachable (TEST-NET-1)
}

TEST_CASE("radio-controller: keeps running and raises an alarm when the radio is unusable") {
    std::vector<std::string> args{"radio-controller",
                                  "-c",
                                  std::string(RADIO_TEST_DATA_DIR) + "/empty.toml",
                                  "--backend",
                                  "iio",
                                  "--uri",
                                  std::string("xml:") + RADIO_TEST_DATA_DIR + "/ad9364.xml",
                                  "-D",
                                  "log.console=false",
                                  "-D",
                                  "http.enabled=false",
                                  "-D",
                                  "forge.data=tcp://127.0.0.1:*",
                                  "-D",
                                  "zmq.tx=tcp://127.0.0.1:*",
                                  "-D",
                                  "forge.control=tcp://127.0.0.1:*"};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());

    radio::RadioController app;
    std::promise<void> started;
    app.hook_start([&](forge::Application& a) { a.post([&] { started.set_value(); }); });
    std::thread runner([&] { CHECK(app.run(static_cast<int>(argv.size()), argv.data()) == 0); });
    started.get_future().get();

    forge::Remote ctl(app.control_endpoint(), 5s);
    CHECK(ctl.get("connected") == false);
    const auto alarms = app.alarms().list();
    REQUIRE(alarms.size() == 1);
    CHECK(alarms[0].id == "radio.device");
    CHECK(alarms[0].severity == forge::Severity::error);
    CHECK(alarms[0].message.find("Radio not available") == 0);
    CHECK(status_of([&] { ctl.set("rx_lo_mhz", 2400); }) == 503);

    app.stop();
    runner.join();
}
#endif

TEST_CASE("radio-controller: typed tune and status commands on the link") {
    RunningRadio r({});
    fz::Client link(r.app.control_endpoint(), 5s);
    radio_status_reply st{};
    REQUIRE(link.call(radio_msg_id_status, radio_status_request{}, st).ok());
    CHECK(st.connected);
    const double before = st.rx_lo_hz;

    radio_tune_request tune{};
    tune.rx_lo_hz = 915.5e6;  // only the LO; the rate and bandwidth stay
    REQUIRE(link.call(radio_msg_id_tune, tune, st).ok());
    CHECK(st.rx_lo_hz == 915.5e6);
    CHECK(st.rx_lo_hz != before);
    forge::Remote ctl(r.app.control_endpoint(), 5s);
    CHECK(ctl.get("rx_lo_mhz") == 915.5);

    tune.rx_lo_hz = 10e9;  // above the AD9364
    const auto refused = link.call(radio_msg_id_tune, tune, st);
    CHECK(refused.errno_code() == EINVAL);
    REQUIRE(link.call(radio_msg_id_status, radio_status_request{}, st).ok());
    CHECK(st.rx_lo_hz == 915.5e6);

    // Tuning changes the radio: it is audited, a status query is not.
    const auto audit = r.app.audit().list();
    CHECK(std::count_if(audit.begin(), audit.end(), [](const auto& a) { return a.target == "tune"; }) == 2);
    CHECK(std::none_of(audit.begin(), audit.end(), [](const auto& a) { return a.target == "status"; }));
}
