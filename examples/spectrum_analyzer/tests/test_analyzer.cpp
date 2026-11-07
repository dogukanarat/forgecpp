// Engine on the simulator and the whole application (ZeroMQ control, spectrum stream, HTTP).
#include "analyzer.hpp"
#include "engine.hpp"

#include <forge/testing.hpp>
#include <forge/zmq.hpp>

#include <atomic>
#include <cmath>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;
using forge::json;
namespace fz = forge::zmq;

namespace {

sa::Engine::BackendFactory sim() {
    return [] {
        radio::SimOptions o;
        o.loopback = false;
        return radio::make_sim_backend(o);
    };
}

std::shared_ptr<const sa::Frame> wait_frame(sa::Engine& e, const std::function<bool(const sa::Frame&)>& ok,
                                            std::chrono::milliseconds timeout = 10s) {
    std::shared_ptr<const sa::Frame> f;
    forge::testing::wait_until([&] {
        f = e.latest();
        return f && ok(*f);
    }, timeout);
    return f && ok(*f) ? f : nullptr;
}

double peak_near(const sa::Frame& f, double hz) {
    for (const auto& p : f.peaks) {
        if (std::abs(p.freq_hz - hz) < 3 * f.plan.rbw_hz + (f.stop_hz - f.start_hz) / f.points) return p.freq_hz;
    }
    return 0;
}

}  // namespace

TEST_CASE("sa engine: realtime span finds the simulated Wi-Fi carriers") {
    sa::Engine e(sim(), {});
    sa::Settings s;
    s.center_hz = 2425e6;
    s.span_hz = 40e6;
    s.update_hz = 30;
    s.settle_buffers = 0;
    e.start(s);
    const auto f = wait_frame(e, [](const sa::Frame& f) { return f.seq >= 3; });
    REQUIRE(f);
    CHECK(e.status().connected);
    CHECK_FALSE(f->plan.swept);
    CHECK(f->points == 1001);
    CHECK(f->live.size() == 1001);
    CHECK(f->start_hz == 2405e6);
    CHECK(f->stop_hz == 2445e6);
    CHECK(peak_near(*f, 2412e6) != 0);
    CHECK(peak_near(*f, 2437e6) != 0);
    CHECK(std::abs(f->peaks.front().freq_hz - 2412e6) < 200e3);  // strongest
    // trace 2 is max hold: never below the live trace
    REQUIRE(f->traces[1].size() == f->live.size());
    for (std::size_t i = 0; i < f->live.size(); ++i) CHECK(f->traces[1][i] >= f->live[i] - 1e-9);
    CHECK(f->traces[2].empty());  // blank
    CHECK_FALSE(e.last_capture().iq.empty());

    // Markers, delta and channel power.
    s.markers[0] = {true, 2412e6, false};
    s.markers[1] = {true, 2437e6, true};
    s.measurement = "channel_power";
    s.channel_bw_hz = 2e6;
    s.center_hz = 2425e6;
    e.update(s);
    const auto g = wait_frame(e, [](const sa::Frame& f) { return f.markers[1].enabled && f.channel_power; });
    REQUIRE(g);
    CHECK(g->markers[0].level > g->markers[1].level);
    CHECK(g->markers[1].dfreq_hz == doctest::Approx(25e6).epsilon(0.001));
    CHECK(g->markers[1].dlevel_db == doctest::Approx(g->markers[1].level - g->markers[0].level));

    // Peak track: marker 1 jumps to the strongest signal.
    s.markers[0] = {true, 2430e6, false};
    s.peak_track = true;
    e.update(s);
    const auto h = wait_frame(e, [](const sa::Frame& f) { return std::abs(f.markers[0].freq_hz - 2412e6) < 200e3; });
    CHECK(h);
    e.stop();
}

TEST_CASE("sa engine: swept span and dBm unit") {
    sa::Engine e(sim(), {});
    sa::Settings s;
    s.center_hz = 750e6;
    s.span_hz = 600e6;  // 450-1050 MHz: the 868.3 and 915 MHz carriers
    s.update_hz = 30;
    s.settle_buffers = 0;
    s.unit = "dBm";
    s.cal_offset_db = 0;
    e.start(s);
    const auto f = wait_frame(e, [](const sa::Frame& f) { return f.seq >= 2; }, 20s);
    REQUIRE(f);
    CHECK(f->plan.swept);
    CHECK(f->plan.segments.size() > 5);
    CHECK(f->unit == "dBm");
    CHECK(f->unit_offset_db == doctest::Approx(-s.rx_gain_db));
    CHECK(peak_near(*f, 868.3e6) != 0);
    CHECK(peak_near(*f, 915e6) != 0);
    CHECK(peak_near(*f, 433.92e6) == 0);  // outside the span
    CHECK(e.last_capture().iq.empty());  // IQ only in realtime spans
    e.stop();
}

TEST_CASE("sa engine: single sweep while stopped") {
    sa::Engine e(sim(), {});
    sa::Settings s;
    s.center_hz = 433.92e6;
    s.span_hz = 2e6;
    s.run = false;
    s.settle_buffers = 0;
    e.start(s);
    std::this_thread::sleep_for(200ms);
    CHECK_FALSE(e.latest());
    e.single();
    const auto f = wait_frame(e, [](const sa::Frame&) { return true; });
    REQUIRE(f);
    std::this_thread::sleep_for(300ms);
    CHECK(e.latest()->seq == f->seq);  // only one
    e.stop();
}

TEST_CASE("sa engine: a failing backend is reported and retried") {
    std::atomic<int> attempts{0};
    sa::Engine e([&]() -> std::unique_ptr<radio::Backend> {
        ++attempts;
        throw radio::DeviceError("no device");
    }, {});
    e.start({});
    REQUIRE(forge::testing::wait_until([&] { return attempts >= 1; }));
    forge::testing::wait_until([&] { return !e.status().error.empty(); });
    CHECK_FALSE(e.status().connected);
    CHECK(e.status().error.find("no device") != std::string::npos);
    e.stop();
}

TEST_CASE("sa app: controls over ZeroMQ, spectrum stream, persistence") {
    forge::testing::TempDir dir;
    const auto state = (dir / "state.json").string();
    std::vector<std::string> args{"spectrum-analyzer",
                                  "--backend", "sim",
                                  "-D", "log.console=false",
                                  "-D", "http.enabled=false",
                                  "-D", "zmq.spectrum=tcp://127.0.0.1:*",
                                  "-D", "zmq.control=tcp://127.0.0.1:*",
                                  "-D", "analyzer.center_hz=2425e6",
                                  "-D", "analyzer.span_hz=40e6",
                                  "-D", "analyzer.update_hz=30",
                                  "-D", "analyzer.state_file=" + state};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());

    sa::Analyzer app;
    std::promise<void> started;
    app.hook_start([&](forge::Application& a) { a.post([&] { started.set_value(); }); });
    std::thread runner([&] { CHECK(app.run(static_cast<int>(argv.size()), argv.data()) == 0); });
    started.get_future().get();

    std::mutex m;
    std::vector<fz::Message> frames;
    fz::Receiver::Options opts;
    opts.topics = {"spectrum"};
    fz::Receiver sub(fz::Receiver::Kind::sub, app.spectrum_endpoint(), [&](fz::Message&& msg) {
        std::lock_guard lock(m);
        frames.push_back(std::move(msg));
    }, opts);
    REQUIRE(forge::testing::wait_until([&] { std::lock_guard lock(m); return frames.size() >= 2; }, 10s));
    {
        std::lock_guard lock(m);
        const auto& h = frames.back().header;
        CHECK(h["format"] == "f32");
        CHECK(h["points"] == 1001);
        CHECK(frames.back().payload.size() == 1001 * sizeof(float));
    }

    fz::Client ctl(app.control_endpoint(), 5s);
    CHECK(forge::testing::wait_until([&] { return ctl.call("get", {{"id", "connected"}})["value"] == true; }));
    CHECK(ctl.call("get", {{"id", "start_mhz"}})["value"] == 2405.0);

    // Peak search puts the active marker on the strongest carrier (2412 MHz).
    CHECK(ctl.call("press", {{"id", "peak_search"}})["ok"] == true);
    CHECK(std::abs(ctl.call("get", {{"id", "m1_mhz"}})["value"].get<double>() - 2412.0) < 0.2);
    CHECK(ctl.call("get", {{"id", "m1_on"}})["value"] == true);
    CHECK(ctl.call("press", {{"id", "peak_right"}})["ok"] == true);
    const double right = ctl.call("get", {{"id", "m1_mhz"}})["value"].get<double>();
    CHECK(right > 2412.2);
    CHECK(ctl.call("press", {{"id", "peak_left"}})["ok"] == true);
    CHECK(ctl.call("get", {{"id", "m1_mhz"}})["value"].get<double>() < right);

    // Marker to center, span / start / stop / presets.
    ctl.call("press", {{"id", "peak_search"}});
    ctl.call("press", {{"id", "marker_to_center"}});
    CHECK(std::abs(ctl.call("get", {{"id", "center_mhz"}})["value"].get<double>() - 2412.0) < 0.2);
    ctl.call("set", {{"id", "start_mhz"}, {"value", 2400}});
    ctl.call("set", {{"id", "stop_mhz"}, {"value", 2450}});
    CHECK(ctl.call("get", {{"id", "center_mhz"}})["value"] == 2425.0);
    CHECK(ctl.call("get", {{"id", "span_mhz"}})["value"] == 50.0);
    auto status_of = [&](const json& req) { return ctl.request(req).value("status", 0); };
    CHECK(status_of({{"cmd", "set"}, {"id", "stop_mhz"}, {"value", 2300}}) == 400);  // below start
    CHECK(status_of({{"cmd", "set"}, {"id", "window"}, {"value", "triangle"}}) == 400);
    ctl.call("set", {{"id", "preset"}, {"value", "ism_433"}});
    CHECK(ctl.call("get", {{"id", "center_mhz"}})["value"] == 433.92);
    CHECK(ctl.call("get", {{"id", "preset"}})["value"] == "ism_433");

    // RBW: manual value, then auto again.
    ctl.call("set", {{"id", "rbw_khz"}, {"value", 1}});
    CHECK(ctl.call("get", {{"id", "rbw_auto"}})["value"] == false);
    ctl.call("set", {{"id", "rbw_auto"}, {"value", true}});
    CHECK(ctl.call("get", {{"id", "rbw_auto"}})["value"] == true);

    // Manual gain is required to set the gain.
    ctl.call("set", {{"id", "gain_mode"}, {"value", "slow_attack"}});
    CHECK(status_of({{"cmd", "set"}, {"id", "rx_gain_db"}, {"value", 20}}) == 409);
    ctl.call("set", {{"id", "gain_mode"}, {"value", "manual"}});
    CHECK(ctl.call("set", {{"id", "rx_gain_db"}, {"value", 20}})["value"] == 20);

    // Limit line below the 433.92 MHz carrier raises an alarm.
    ctl.call("set", {{"id", "limit_level"}, {"value", -100}});
    ctl.call("set", {{"id", "limit_on"}, {"value", true}});
    json alarms;
    const bool limit_raised = forge::testing::wait_until([&] {
        alarms = ctl.call("alarms")["alarms"];
        for (const auto& a : alarms) {
            if (a["id"] == "analyzer.limit" && a["active"] == true) return true;
        }
        return false;
    });
    CAPTURE(alarms.dump());
    CAPTURE(app.latest() ? app.latest()->seq : 0);
    CHECK(limit_raised);
    ctl.call("set", {{"id", "limit_on"}, {"value", false}});

    // Frame contents through the application.
    REQUIRE(forge::testing::wait_until([&] {
        const auto f = app.latest();
        return f && f->start_hz == 432.92e6;
    }));

    app.stop();
    runner.join();

    const auto saved = forge::load_json(state);
    CHECK(saved["center_hz"] == 433.92e6);
    CHECK(saved["span_hz"] == 2e6);
    CHECK(saved["rx_gain_db"] == 20);
}
