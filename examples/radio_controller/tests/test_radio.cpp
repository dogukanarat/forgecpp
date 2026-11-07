#include "backend.hpp"
#include "dsp.hpp"
#include "radio_controller.hpp"

#include <forge/testing.hpp>
#include <forge/zmq.hpp>

#include <cmath>
#include <complex>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using forge::json;
namespace fz = forge::zmq;

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
    o.rx_channels = 2;
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
    b->start_rx(n, 2);
    REQUIRE(b->read_rx(rx1.data(), rx2.data(), n) == n);
    REQUIRE(b->read_rx(rx1.data(), rx2.data(), n) == n);
    const auto s1 = radio::dsp::spectrum_dbfs(rx1.data(), n);
    const auto s2 = radio::dsp::spectrum_dbfs(rx2.data(), n);
    auto level_at = [&](const std::vector<float>& sp, double offset) {
        const auto i = static_cast<std::size_t>(std::lround(offset / 10e6 * n + n / 2.0));
        return *std::max_element(sp.begin() + static_cast<std::ptrdiff_t>(i) - 3, sp.begin() + static_cast<std::ptrdiff_t>(i) + 4);
    };
    // same air on both channels, 20 dB apart (gain)
    CHECK(level_at(s1, 2e6) - level_at(s2, 2e6) == doctest::Approx(20).epsilon(0.05));
    // the loopback tone is on RX2 only
    CHECK(level_at(s2, 1e6) > -40);
    CHECK(level_at(s1, 1e6) < -80);
    // RX1 only: RX2 pointer is ignored
    b->start_rx(n, 1);
    CHECK(b->read_rx(rx1.data(), n) == n);
}

TEST_CASE("radio: single-RX parts reject RX2") {
    auto b = radio::make_sim_backend();  // AD9364
    CHECK(b->rx_channel_count() == 1);
    CHECK_THROWS_AS(b->start_rx(4096, 2), radio::DeviceError);
    CHECK_THROWS_AS(b->set_gain_mode("manual", 1), radio::DeviceError);
    CHECK_THROWS_AS(b->rx_gain(1), radio::DeviceError);
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
                                  "-c", std::string(RADIO_TEST_DATA_DIR) + "/empty.toml",
                                  "--backend", "sim",
                                  "-D", "log.console=false",
                                  "-D", "http.enabled=false",
                                  "-D", "zmq.rx=tcp://127.0.0.1:*",
                                  "-D", "zmq.tx=tcp://127.0.0.1:*",
                                  "-D", "zmq.rx2=tcp://127.0.0.1:*",
                                  "-D", "zmq.control=tcp://127.0.0.1:*",
                                  "-D", "radio.rx_lo_hz=2410e6",
                                  "-D", "radio.tx_lo_hz=2410e6",
                                  "-D", "radio.sample_rate_hz=5e6",
                                  "-D", "radio.rx_bandwidth_hz=5e6",
                                  "-D", "radio.buffer_size=16384",
                                  "-D", "radio.state_file=" + state};
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
    fz::Receiver sub(fz::Receiver::Kind::sub, app.rx_endpoint(), [&](fz::Message&& msg) {
        std::lock_guard lock(m);
        if (frames.size() < 64) frames.push_back(std::move(msg));
    }, opts);
    REQUIRE(forge::testing::wait_until([&] { std::lock_guard lock(m); return frames.size() >= 3; }, 10s));
    {
        std::lock_guard lock(m);
        const auto& h = frames.back().header;
        CHECK(h["format"] == "cs16");
        CHECK(h["samples"] == 16384);
        CHECK(h["sample_rate"] == 5e6);
        CHECK(h["center_freq"] == 2410e6);
        CHECK(frames.back().payload.size() == 16384 * 4);
        CHECK(frames[1].header["seq"].get<int>() > frames[0].header["seq"].get<int>());
    }

    // --- control over ZeroMQ ---------------------------------------------------
    fz::Client ctl(app.control_endpoint(), 5s);
    CHECK(ctl.call("get", {{"id", "connected"}})["value"] == true);
    CHECK(ctl.call("set", {{"id", "rx_lo_mhz"}, {"value", 2411.5}})["value"] == 2411.5);
    auto status_of = [&](const json& req) { return ctl.request(req).value("status", 0); };
    CHECK(status_of({{"cmd", "set"}, {"id", "rx_gain_db"}, {"value", 20}}) == 409);  // AGC owns the gain
    CHECK(status_of({{"cmd", "set"}, {"id", "rx_lo_mhz"}, {"value", 9000}}) == 400);  // above 6 GHz
    ctl.call("set", {{"id", "gain_mode"}, {"value", "manual"}});
    CHECK(ctl.call("set", {{"id", "rx_gain_db"}, {"value", 20}})["value"] == 20);

    auto latest = [&]() {
        std::lock_guard lock(m);
        frames.clear();
        return 0;
    };
    latest();
    REQUIRE(forge::testing::wait_until([&] {
        std::lock_guard lock(m);
        return !frames.empty() && frames.back().header["center_freq"] == 2411.5e6;
    }, 10s));

    // --- TX: disabled drops, enabled loops back into RX --------------------------
    fz::Sender push(fz::Sender::Kind::push, app.tx_endpoint(), false);
    const auto tx = tone(4000, 1.25e6, 5e6, 0.5);
    REQUIRE(forge::testing::wait_until([&] { return push.send("tx", {{"format", "cs16"}}, tx.data(), tx.size() * 2); }));
    CHECK(forge::testing::wait_until([&] { return ctl.call("get", {{"id", "tx_dropped"}})["value"] == 1; }));

    ctl.call("set", {{"id", "tx_enabled"}, {"value", true}});
    ctl.call("set", {{"id", "tx_lo_mhz"}, {"value", 2411.5}});
    ctl.call("set", {{"id", "tx_atten_db"}, {"value", 0}});
    REQUIRE(push.send("tx", {{"format", "cs16"}, {"cyclic", true}}, tx.data(), tx.size() * 2));
    CHECK(forge::testing::wait_until([&] { return ctl.call("get", {{"id", "tx_mode"}})["value"] == "cyclic"; }));

    latest();
    REQUIRE(forge::testing::wait_until([&] {
        std::lock_guard lock(m);
        if (frames.empty()) return false;
        const auto& p = frames.back().payload;
        std::vector<std::int16_t> iq(p.size() / 2);
        std::memcpy(iq.data(), p.data(), p.size());
        return std::abs(peak_offset(iq, 8192, 5e6) - 1.25e6) < 5e3;
    }, 10s));

    ctl.call("press", {{"id", "tx_stop"}});
    CHECK(ctl.call("get", {{"id", "tx_mode"}})["value"] == "idle");

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
        args = {"radio-controller", "-c", std::string(RADIO_TEST_DATA_DIR) + "/empty.toml", "--backend", "sim",
                "-D", "log.console=false", "-D", "http.enabled=false",
                "-D", "zmq.rx=tcp://127.0.0.1:*", "-D", "zmq.tx=tcp://127.0.0.1:*",
                "-D", "zmq.rx2=tcp://127.0.0.1:*", "-D", "zmq.control=tcp://127.0.0.1:*"};
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

TEST_CASE("radio-controller: RX2 blocks for the spectrum analyzer (AD9361, TX1 -> RX2 cable)") {
    RunningRadio r({"-D", "radio.sim.rx_channels=2", "-D", "radio.sim.loopback_rx=2",
                    "-D", "radio.rx2_enabled=true", "-D", "radio.rx2_tap_samples=16384", "-D", "radio.rx2_tap_hz=50",
                    "-D", "radio.rx_lo_hz=2410e6", "-D", "radio.tx_lo_hz=2410e6",
                    "-D", "radio.sample_rate_hz=5e6", "-D", "radio.rx_bandwidth_hz=5e6",
                    "-D", "radio.buffer_size=32768"});
    std::mutex m;
    std::vector<fz::Message> rx2, rx1;
    fz::Receiver::Options opts;
    opts.topics = {"iq"};
    fz::Receiver sub2(fz::Receiver::Kind::sub, r.app.rx2_endpoint(), [&](fz::Message&& msg) {
        std::lock_guard lock(m);
        if (rx2.size() < 400) rx2.push_back(std::move(msg));
    }, opts);
    fz::Receiver sub1(fz::Receiver::Kind::sub, r.app.rx_endpoint(), [&](fz::Message&& msg) {
        std::lock_guard lock(m);
        if (rx1.size() < 400) rx1.push_back(std::move(msg));
    }, opts);
    REQUIRE(forge::testing::wait_until([&] { std::lock_guard lock(m); return rx2.size() >= 2; }, 10s));
    {
        std::lock_guard lock(m);
        const auto& h = rx2.back().header;
        CHECK(h["channel"] == 2);
        CHECK(h["samples"] == 16384);  // the start of a 32768-sample DMA buffer
        CHECK(h["format"] == "cs16");
        CHECK(h["center_freq"] == 2410e6);
        CHECK(h["sample_rate"] == 5e6);
        CHECK(rx2.back().payload.size() == 16384 * 4);
        CHECK(rx2[1].header["seq"].get<int>() > rx2[0].header["seq"].get<int>());
    }

    fz::Client ctl(r.app.control_endpoint(), 5s);
    CHECK(ctl.call("get", {{"id", "rx_channels"}})["value"] == 2);
    ctl.call("set", {{"id", "rx2_gain_mode"}, {"value", "manual"}});
    CHECK(ctl.call("set", {{"id", "rx2_gain_db"}, {"value", 25}})["value"] == 25);
    auto newest = [&](std::vector<fz::Message>& v) {
        std::lock_guard lock(m);
        return v.empty() ? fz::Message{} : v.back();
    };
    auto clear = [&] {
        std::lock_guard lock(m);
        rx1.clear();
        rx2.clear();
    };
    clear();
    REQUIRE(forge::testing::wait_until([&] { return newest(rx2).header.value("rx_gain_db", 0.0) == 25.0; }));

    // A cyclic tone on TX1 shows up on RX2 (cable) but not on RX1.
    ctl.call("set", {{"id", "tx_enabled"}, {"value", true}});
    ctl.call("set", {{"id", "tx_atten_db"}, {"value", 0}});
    fz::Sender push(fz::Sender::Kind::push, r.app.tx_endpoint(), false);
    const auto tx = tone(4000, 1.25e6, 5e6, 0.5);
    REQUIRE(forge::testing::wait_until([&] { return push.send("tx", {{"format", "cs16"}, {"cyclic", true}}, tx.data(), tx.size() * 2); }));
    auto peak_of = [&](const fz::Message& msg, std::size_t n) {
        std::vector<std::int16_t> iq(msg.payload.size() / 2);
        std::memcpy(iq.data(), msg.payload.data(), msg.payload.size());
        return peak_offset(iq, n, 5e6);
    };
    clear();
    CHECK(forge::testing::wait_until([&] {
        const auto b = newest(rx2);
        return !b.payload.empty() && std::abs(peak_of(b, 16384) - 1.25e6) < 5e3;
    }, 10s));
    clear();
    REQUIRE(forge::testing::wait_until([&] { return !newest(rx1).payload.empty(); }));
    CHECK(std::abs(peak_of(newest(rx1), 8192) - 2e6) < 5e3);  // RX1: the strongest is the 2412 MHz carrier

    // Retuning (radio-controller owns the shared RX LO) is visible in the headers.
    ctl.call("set", {{"id", "rx_lo_mhz"}, {"value", 2411}});
    CHECK(forge::testing::wait_until([&] { return newest(rx2).header.value("center_freq", 0.0) == 2411e6; }));

    // RX2 off: no more blocks.
    ctl.call("set", {{"id", "rx2_enabled"}, {"value", false}});
    std::this_thread::sleep_for(300ms);
    clear();
    std::this_thread::sleep_for(300ms);
    CHECK(newest(rx2).payload.empty());
}

TEST_CASE("radio-controller: RX2 on an AD9364 is refused with an explanation") {
    RunningRadio r({"-D", "radio.rx2_enabled=true"});  // simulated AD9364 (one RX channel)
    fz::Client ctl(r.app.control_endpoint(), 5s);
    CHECK(ctl.call("get", {{"id", "rx_channels"}})["value"] == 1);
    const auto alarms = ctl.call("alarms")["alarms"];
    REQUIRE(alarms.size() == 1);
    CHECK(alarms[0]["id"] == "radio.rx2");
    const auto reply = ctl.request({{"cmd", "set"}, {"id", "rx2_enabled"}, {"value", true}});
    CHECK(reply["status"] == 409);
    CHECK(reply["error"].get<std::string>().find("AD9361/AD9363") != std::string::npos);
    CHECK(ctl.request({{"cmd", "set"}, {"id", "rx2_gain_mode"}, {"value", "manual"}})["status"] == 502);
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
    CHECK_THROWS_AS(two->rx_gain(1), radio::DeviceError);  // channel exists, XML attributes are unreadable

    auto one = radio::make_iio_backend(std::string("xml:") + RADIO_TEST_DATA_DIR + "/ad9364.xml", 1000);
    CHECK(one->rx_channel_count() == 1);
    try {
        one->start_rx(4096, 2);
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
    std::vector<std::string> args{"radio-controller", "-c", std::string(RADIO_TEST_DATA_DIR) + "/empty.toml",
                                  "--backend", "iio",
                                  "--uri", std::string("xml:") + RADIO_TEST_DATA_DIR + "/ad9364.xml",
                                  "-D", "log.console=false", "-D", "http.enabled=false",
                                  "-D", "zmq.rx=tcp://127.0.0.1:*", "-D", "zmq.tx=tcp://127.0.0.1:*",
                                  "-D", "zmq.rx2=tcp://127.0.0.1:*", "-D", "zmq.control=tcp://127.0.0.1:*"};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());

    radio::RadioController app;
    std::promise<void> started;
    app.hook_start([&](forge::Application& a) { a.post([&] { started.set_value(); }); });
    std::thread runner([&] { CHECK(app.run(static_cast<int>(argv.size()), argv.data()) == 0); });
    started.get_future().get();

    fz::Client ctl(app.control_endpoint(), 5s);
    CHECK(ctl.call("get", {{"id", "connected"}})["value"] == false);
    const auto alarms = ctl.call("alarms")["alarms"];
    REQUIRE(alarms.size() == 1);
    CHECK(alarms[0]["id"] == "radio.device");
    CHECK(alarms[0]["severity"] == "error");
    CHECK(alarms[0]["message"].get<std::string>().find("Radio not available") == 0);
    CHECK(ctl.request({{"cmd", "set"}, {"id", "rx_lo_mhz"}, {"value", 2400}})["status"] == 503);

    app.stop();
    runner.join();
}
#endif
