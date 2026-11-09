// Analyzer on RX2 of radio-controller (AD9361/AD9363 shared by both daemons),
// and the bounded capture size (CMA-friendly DMA buffers).
#include "engine.hpp"
#include "radio_tap.hpp"

#include <forge/testing.hpp>
#include <forge/zmq.hpp>

#include "radio/radio.forge.hpp"

#ifdef SA_WITH_RADIO_CONTROLLER
#include "radio_controller.hpp"
#endif

#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;
using forge::json;
namespace fz = forge::zmq;

namespace {

/// Stand-in for radio-controller: the get/set commands of its control
/// endpoint and the RX2 stream.
class FakeRadio {
public:
    explicit FakeRadio(json tap_channels = json::array({2})) {
        state_ = {{"tap_channels", tap_channels}, {"rx2_gain_mode", "slow_attack"},
                  {"rx2_gain_db", 30.0},          {"gain_mode", "slow_attack"},
                  {"rx_gain_db", 30.0},           {"temperature_c", 44.5}};
        commands_.add<forge_get_request, forge_get_reply>(
            forge_msg_id_get, "get", [this](const auto& req, auto& rsp) { return answer(req.id, nullptr, rsp.value); });
        commands_.add<forge_set_request, forge_set_reply>(forge_msg_id_set, "set", [this](const auto& req, auto& rsp) {
            return answer(req.id, req.value, rsp.value);
        });
        control_ = std::make_unique<fz::Server>("tcp://127.0.0.1:*",
                                                [this](std::string_view req) { return fz::answer(commands_, req); });
        pub_ = std::make_unique<fz::Publisher>("tcp://127.0.0.1:*");
        stream_ = &pub_->add<radio_iq_block>("rx2");
        thread_ = std::thread([this] { publish(); });
    }
    ~FakeRadio() {
        running_ = false;
        thread_.join();
    }
    std::string control() const { return control_->endpoint(); }
    std::string data() const { return pub_->endpoint(); }
    json state() {
        std::lock_guard lock(m_);
        return state_;
    }
    std::atomic<int> sets{0};
    std::atomic<double> lo{915e6};
    std::atomic<double> tone_offset{1.5e6};
    static constexpr double kRate = 10e6;
    static constexpr std::size_t kBlock = 16384;

private:
    /// A get (value == nullptr) or a set; the control's value goes to `out`.
    template <std::size_t N>
    forge::Status answer(const char* id, const char* value, char (&out)[N]) {
        std::lock_guard lock(m_);
        if (!state_.contains(id)) return forge::Status::error(forge::module_id::app, ENOENT, "unknown control");
        if (value) {
            state_[id] = json::parse(value);
            ++sets;
        }
        return forge::pb::assign(out, state_[id].dump());
    }
    void publish() {
        std::vector<std::int16_t> iq(2 * kBlock);
        std::uint64_t seq = 0;
        double phase = 0;
        while (running_) {
            const double w = 2 * M_PI * tone_offset / kRate;
            for (std::size_t i = 0; i < kBlock; ++i, phase += w) {
                iq[2 * i] = static_cast<std::int16_t>(8000 * std::cos(phase) + (i * 7919 % 64) - 32);
                iq[2 * i + 1] = static_cast<std::int16_t>(8000 * std::sin(phase) + (i * 104729 % 64) - 32);
            }
            radio_iq_block b{};
            {
                std::lock_guard lock(m_);
                b.rx_gain_db = state_["rx2_gain_db"].get<double>();
            }
            b.channel = 2;
            b.samples = kBlock;
            std::strcpy(b.format, "cs16");
            b.sample_rate = kRate;
            b.center_freq = lo.load();
            b.buffer = ++seq;
            stream_->publish(b, nullptr, iq.data(), iq.size() * sizeof(std::int16_t));  // no owner: copied
            std::this_thread::sleep_for(20ms);
        }
    }

    std::mutex m_;
    json state_;
    forge::tmtc::Registry commands_;
    std::unique_ptr<fz::Server> control_;
    std::unique_ptr<fz::Publisher> pub_;
    fz::Stream* stream_ = nullptr;
    std::atomic<bool> running_{true};
    std::thread thread_;
};

/// Peak positions are quantized to display points (and limited by the RBW).
double tolerance(const sa::Frame& f) {
    return 2 * std::max(f.plan.rbw_hz, (f.stop_hz - f.start_hz) / static_cast<double>(f.points - 1));
}

std::shared_ptr<const sa::Frame> wait_frame(sa::Engine& e, const std::function<bool(const sa::Frame&)>& ok,
                                            std::chrono::milliseconds timeout = 10s) {
    std::shared_ptr<const sa::Frame> f;
    forge::testing::wait_until(
        [&] {
            f = e.latest();
            return f && ok(*f);
        },
        timeout);
    return f && ok(*f) ? f : nullptr;
}

sa::RadioTapOptions tap_of(const FakeRadio& r) {
    sa::RadioTapOptions o;
    o.data = r.data();
    o.control = r.control();
    o.timeout = 2s;
    return o;
}

}  // namespace

TEST_CASE("sa radio tap: the window follows the band radio-controller is tuned to") {
    FakeRadio radio;
    sa::Engine e([o = tap_of(radio)] { return sa::make_radio_tap_backend(o); }, {});
    sa::Settings s;
    s.center_hz = 2440e6;  // not in the band: the whole band is shown
    s.span_hz = 100e6;
    s.gain_mode = "manual";
    s.rx_gain_db = 33;
    s.unit = "dBm";
    s.update_hz = 30;
    e.start(s);

    auto f = wait_frame(e, [](const sa::Frame& f) { return f.seq >= 2; });
    CAPTURE(e.status().error);
    REQUIRE(f);
    CHECK(f->plan.fixed);
    CHECK_FALSE(f->plan.swept);
    CHECK(f->start_hz == doctest::Approx(911e6));  // 915 MHz +- 10 MSPS * 0.8 / 2
    CHECK(f->stop_hz == doctest::Approx(919e6));
    CHECK(f->plan.fft_size <= FakeRadio::kBlock);
    REQUIRE_FALSE(f->peaks.empty());
    CHECK(std::abs(f->peaks.front().freq_hz - 916.5e6) < tolerance(*f));
    // The analyzer owns the RX2 gain; dBm uses the gain radio-controller reports.
    CHECK(radio.state()["rx2_gain_mode"] == "manual");
    CHECK(radio.state()["gain_mode"] == "slow_attack");  // RX1 untouched
    CHECK(radio.state()["rx2_gain_db"] == 33.0);
    REQUIRE(forge::testing::wait_until([&] {
        auto g = e.latest();
        return g && g->rx_gain_db == 33.0;
    }));
    CHECK(e.latest()->unit_offset_db == doctest::Approx(-33.0));
    CHECK(e.status().connected);
    CHECK(e.status().temperature_c.value_or(0) == 44.5);

    // Zoom inside the band
    s.center_hz = 916.5e6;
    s.span_hz = 1e6;
    s.rbw_hz = 2e3;
    e.update(s);
    f = wait_frame(e, [](const sa::Frame& f) { return f.start_hz == 916e6; });
    REQUIRE(f);
    CHECK(f->stop_hz == 917e6);
    CHECK(f->plan.rbw_hz <= 2e3 * 1.01 + 1e3);  // limited by the block size

    // radio-controller retunes somewhere else: the window jumps to the new band, traces restart
    radio.lo = 2400e6;
    f = wait_frame(e, [](const sa::Frame& f) { return f.start_hz == 2396e6; });
    REQUIRE(f);
    CHECK(f->stop_hz == 2404e6);
    CHECK(std::abs(f->peaks.front().freq_hz - 2401.5e6) < tolerance(*f));
    e.stop();
}

TEST_CASE("sa radio tap: radio-controller must share the channel") {
    FakeRadio radio(json::array());  // tap_rx = [] in its configuration
    sa::Engine e([o = tap_of(radio)] { return sa::make_radio_tap_backend(o); }, {});
    e.start({});
    REQUIRE(forge::testing::wait_until([&] { return !e.status().error.empty(); }));
    CHECK_FALSE(e.status().connected);
    CHECK(e.status().error.find("does not share RX2") != std::string::npos);
    CHECK(e.status().error.find("tap_rx") != std::string::npos);
    e.stop();
}

TEST_CASE("sa radio tap: control_gain = false leaves the channel's gain alone") {
    FakeRadio radio;
    auto o = tap_of(radio);
    o.control_gain = false;
    sa::Engine::Options eo;
    eo.control_gain = false;
    sa::Engine e([o] { return sa::make_radio_tap_backend(o); }, {}, eo);
    sa::Settings s;
    s.gain_mode = "manual";
    s.rx_gain_db = 10;
    s.update_hz = 30;
    e.start(s);
    REQUIRE(wait_frame(e, [](const sa::Frame& f) { return f.seq >= 2; }));
    CHECK(radio.sets == 0);
    CHECK(radio.state()["rx2_gain_db"] == 30.0);
    CHECK(e.latest()->rx_gain_db == 30.0);  // reported by radio-controller
    e.stop();
}

TEST_CASE("sa radio tap: no radio-controller") {
    sa::RadioTapOptions o;
    o.control = "tcp://127.0.0.1:9";  // nothing there
    o.timeout = 300ms;
    CHECK_THROWS_AS(sa::make_radio_tap_backend(o), radio::DeviceError);
}

namespace {
/// Records the largest DMA buffer requested from the simulator.
class Recorder final : public radio::Backend {
public:
    explicit Recorder(std::atomic<std::size_t>& largest) : inner_(radio::make_sim_backend()), largest_(largest) {}
    std::string name() const override { return inner_->name(); }
    nlohmann::json describe() const override { return inner_->describe(); }
    void set_rx_lo(double v) override { inner_->set_rx_lo(v); }
    void set_tx_lo(double v) override { inner_->set_tx_lo(v); }
    void set_sample_rate(double v) override { inner_->set_sample_rate(v); }
    void set_rx_bandwidth(double v) override { inner_->set_rx_bandwidth(v); }
    void set_tx_bandwidth(double v) override { inner_->set_tx_bandwidth(v); }
    void set_gain_mode(const std::string& m, unsigned c) override { inner_->set_gain_mode(m, c); }
    void set_rx_gain(double v, unsigned c) override { inner_->set_rx_gain(v, c); }
    void set_tx_attenuation(double v, unsigned c) override { inner_->set_tx_attenuation(v, c); }
    double rx_gain(unsigned c) override { return inner_->rx_gain(c); }
    std::optional<double> temperature_c() override { return inner_->temperature_c(); }
    unsigned rx_channel_count() const override { return inner_->rx_channel_count(); }
    unsigned tx_channel_count() const override { return inner_->tx_channel_count(); }
    void start_rx(std::size_t n, unsigned c) override {
        largest_ = std::max(largest_.load(), n);
        inner_->start_rx(n, c);
    }
    std::size_t read_rx(std::int16_t* a, std::int16_t* b, std::size_t n) override { return inner_->read_rx(a, b, n); }
    void stop_rx() override { inner_->stop_rx(); }
    void cancel_rx() override { inner_->cancel_rx(); }
    bool take_rx_overflow() override { return inner_->take_rx_overflow(); }
    void start_tx(std::size_t n, bool c, unsigned ch) override { inner_->start_tx(n, c, ch); }
    void write_tx(const std::int16_t* p, std::size_t n) override { inner_->write_tx(p, n); }
    void stop_tx() override { inner_->stop_tx(); }
    bool take_tx_underflow() override { return inner_->take_tx_underflow(); }

private:
    std::unique_ptr<radio::Backend> inner_;
    std::atomic<std::size_t>& largest_;
};
}  // namespace

TEST_CASE("sa engine: listens to RX2 of its own transceiver") {
    radio::SimOptions so;
    so.channels = 2;
    so.loopback = false;
    sa::Engine::Options eo;
    eo.rx_channel = 1;
    sa::Engine e([so] { return radio::make_sim_backend(so); }, {}, eo);
    sa::Settings s;
    s.center_hz = 868e6;
    s.span_hz = 4e6;
    s.gain_mode = "manual";
    s.rx_gain_db = 20;
    s.settle_buffers = 0;
    s.update_hz = 30;
    e.start(s);
    const auto f = wait_frame(e, [](const sa::Frame& f) { return f.seq >= 2; });
    REQUIRE(f);
    CHECK(f->rx_gain_db == 20);
    CHECK(std::abs(f->peaks.front().freq_hz - 868.3e6) < tolerance(*f));
    e.stop();
}

TEST_CASE("sa engine: narrow RBW with heavy averaging reads bounded DMA buffers") {
    std::atomic<std::size_t> largest{0};
    sa::Engine e([&] { return std::make_unique<Recorder>(largest); }, {});
    sa::Settings s;
    s.center_hz = 433.92e6;
    s.span_hz = 200e3;
    s.rbw_hz = 50;          // -> 65536-point FFT at 2.08 MSPS
    s.vbw_averages = 8;     // 512 k samples in one go before; now chunks of kMaxBufferSamples
    s.settle_buffers = 0;
    e.start(s);
    const auto f = wait_frame(e, [](const sa::Frame&) { return true; }, 20s);
    REQUIRE(f);
    CHECK(f->plan.fft_size == 65536);
    CHECK(largest <= sa::kMaxBufferSamples);
    CHECK(largest >= 65536);
    REQUIRE_FALSE(f->peaks.empty());
    CHECK(std::abs(f->peaks.front().freq_hz - 433.92e6) < tolerance(*f));
    e.stop();
}

#ifdef SA_WITH_RADIO_CONTROLLER
TEST_CASE("sa radio tap: with the real radio-controller (simulated AD9361, TX1 -> RX2 cable)") {
    forge::testing::TempDir dir;
    const auto empty = (dir / "empty.toml").string();
    { std::ofstream(empty) << "# no radio-controller.toml from the working directory\n"; }
    std::vector<std::string> args{"radio-controller",
                                  "-c",
                                  empty,
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
                                  "radio.sim.channels=2",
                                  "-D",
                                  "radio.sim.loopback_rx=2",
                                  "-D",
                                  "radio.tap_rx=[2]",
                                  "-D",
                                  "radio.rx_lo_hz=2410e6",
                                  "-D",
                                  "radio.tx_lo_hz=2410e6",
                                  "-D",
                                  "radio.sample_rate_hz=10e6",
                                  "-D",
                                  "radio.rx_bandwidth_hz=10e6",
                                  "-D",
                                  "radio.tx_enabled=true",
                                  "-D",
                                  "radio.tx_attenuation_db=10"};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    radio::RadioController app;
    std::promise<void> started;
    app.hook_start([&](forge::Application& a) { a.post([&] { started.set_value(); }); });
    std::thread runner([&] { CHECK(app.run(static_cast<int>(argv.size()), argv.data()) == 0); });
    started.get_future().get();

    sa::RadioTapOptions o;
    o.data = app.data_endpoint();
    o.control = app.control_endpoint();
    sa::Engine e([o] { return sa::make_radio_tap_backend(o); }, {});
    sa::Settings s;
    s.update_hz = 30;
    e.start(s);
    // RX2 shared by radio-controller; the 2412 MHz carrier is in the band
    auto f = wait_frame(e, [](const sa::Frame& f) { return f.plan.fixed && f.seq >= 2; }, 15s);
    REQUIRE(f);
    CHECK(f->start_hz == doctest::Approx(2406e6));
    CHECK(f->stop_hz == doctest::Approx(2414e6));
    CHECK(std::abs(f->peaks.front().freq_hz - 2412e6) < tolerance(*f));

    // A tone sent on TX1 appears on RX2 at TX LO + 1.5 MHz
    std::vector<std::int16_t> tx(2 * 4000);
    for (std::size_t i = 0; i < 4000; ++i) {
        const double ph = 2 * M_PI * 1.5e6 * static_cast<double>(i) / 10e6;
        tx[2 * i] = static_cast<std::int16_t>(16000 * std::cos(ph));
        tx[2 * i + 1] = static_cast<std::int16_t>(16000 * std::sin(ph));
    }
    fz::Sender push(fz::Sender::Kind::push, app.tx_endpoint(), false);
    radio_tx_block tb{};
    std::strcpy(tb.format, "cs16");
    tb.cyclic = true;
    REQUIRE(forge::testing::wait_until([&] { return push.publish("tx", tb, tx.data(), tx.size() * 2); }));
    f = wait_frame(
        e,
        [](const sa::Frame& f) {
            return !f.peaks.empty() && std::abs(f.peaks.front().freq_hz - 2411.5e6) < tolerance(f);
        },
        15s);
    CHECK(f);
    e.stop();
    app.stop();
    runner.join();
}
#endif
