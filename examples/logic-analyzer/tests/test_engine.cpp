// The engine (triggers, pre-trigger, limits, gaps) and the files (.lacap, VCD, CSV, the store).
#include "decoders.hpp"
#include "engine.hpp"
#include "files.hpp"
#include "sim.hpp"

#include <forge/testing.hpp>

#include <atomic>
#include <fstream>
#include <sstream>

using namespace la;
using namespace std::chrono_literals;

namespace {

/// Runs one capture to the end and returns it.
struct Run {
    std::atomic<int> done{0};
    std::shared_ptr<SharedCapture> last;
    std::mutex m;
    Engine engine;

    explicit Run(std::unique_ptr<Source> src)
        : engine(std::move(src), [this](std::shared_ptr<SharedCapture> c) {
              {
                  std::lock_guard lock(m);
                  last = std::move(c);
              }
              ++done;
          }) {}

    Capture capture(const CaptureSettings& s, int expect = 1) {
        engine.start(s);
        REQUIRE(forge::testing::wait_until([&] { return done.load() >= expect; }, 20s));
        std::lock_guard lock(m);
        return last->read([](const Capture& c) { return c; });
    }
};

std::unique_ptr<Source> sim(const std::string& scenario) {
    return std::make_unique<SimSource>(make_scenario(scenario), false, 5ms);
}

/// Words from a script, for what the simulator does not do (lost words).
class ScriptSource : public Source {
public:
    explicit ScriptSource(std::vector<std::uint32_t> words) : words_(std::move(words)) {}
    std::string name() const override { return "script"; }
    void start(const SourceConfig&) override { pos_ = 0; }
    void stop() override {}
    std::size_t read(std::vector<std::uint32_t>& out, std::chrono::milliseconds) override {
        const std::size_t n = std::min<std::size_t>(3, words_.size() - pos_);
        out.insert(out.end(), words_.begin() + static_cast<std::ptrdiff_t>(pos_),
                   words_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        if (n == 0) std::this_thread::sleep_for(1ms);
        return n;
    }
    SourceStats stats() const override { return {}; }

private:
    std::vector<std::uint32_t> words_;
    std::size_t pos_ = 0;
};

}  // namespace

TEST_CASE("engine: a timed capture without a trigger") {
    Run r(sim("mixed"));
    const Capture c = r.capture({.sample_hz = 100e6, .duration_s = 0.03});
    CHECK(c.done);
    CHECK(c.begin == 0);
    CHECK(c.end == 3'000'000);
    CHECK_FALSE(c.trigger);
    CHECK(c.size() > 300);  // two UART lines, three I2C reads, 30 PWM periods
    CHECK(c.source == "sim:mixed");
    for (std::size_t i = 1; i < c.size(); ++i) REQUIRE(c.t[i] > c.t[i - 1]);
    const auto uart = make_decoder("uart", normalize_settings("uart", {{"channel", 0}}))->decode(c);
    REQUIRE(uart.size() > 10);
    CHECK(uart[0].value == 'H');
    CHECK(r.engine.status().stop_reason == "duration");
    CHECK(r.engine.status().state == State::idle);
}

TEST_CASE("engine: an edge trigger keeps the pre-trigger time and records the duration after it") {
    Run r(sim("spi"));
    // The first CS falling edge (the SPI transaction at 0.2 ms), 0.1 ms kept before it.
    const Capture c = r.capture(
        {.sample_hz = 100e6, .duration_s = 0.002, .trigger = "falling", .trigger_channel = 0, .pre_trigger_ms = 0.1});
    REQUIRE(c.trigger);
    CHECK(*c.trigger == 20'000);  // 0.2 ms at 100 MHz
    CHECK(c.begin == 10'000);
    CHECK(c.end == 20'000 + 200'000);
    CHECK_FALSE(c.level(0, *c.trigger));
    CHECK(c.level(0, *c.trigger - 1));
    CHECK(c.origin() == *c.trigger);
    CHECK(c.time_of(c.begin) == doctest::Approx(-0.0001));
}

TEST_CASE("engine: the trigger is armed only once the pre-trigger time is recorded") {
    Run r(sim("spi"));
    // The first CS edge (0.2 ms) comes before 0.3 ms of pre-trigger: the next one (5.2 ms) fires.
    const Capture c = r.capture({.sample_hz = 100e6, .duration_s = 0.001, .trigger = "falling", .pre_trigger_ms = 0.3});
    REQUIRE(c.trigger);
    CHECK(*c.trigger == 520'000);
    CHECK(c.begin == 520'000 - 30'000);
}

TEST_CASE("engine: a pattern trigger, and pre-trigger time that is dropped while waiting") {
    Run r(sim("mixed"));
    // UART TX low (a start bit) while SCL and SDA are both high: the UART starts at 1 ms, idle I2C.
    const Capture c = r.capture(
        {.sample_hz = 10e6, .duration_s = 0.001, .trigger = "pattern", .pattern = "011X", .pre_trigger_ms = 0.05});
    REQUIRE(c.trigger);
    CHECK((c.levels_at(*c.trigger) & 0b0111) == 0b0110);
    CHECK((c.before(c.lower(*c.trigger)) & 0b0111) != 0b0110);
    CHECK(c.begin == *c.trigger - 500);  // 0.05 ms at 10 MHz
    for (const auto t : c.t) CHECK(t >= c.begin);
}

TEST_CASE("engine: until stopped, and the edge limit") {
    Run r(sim("mixed"));
    r.engine.start({.sample_hz = 100e6, .duration_s = 0});
    REQUIRE(forge::testing::wait_until([&] {
        auto cur = r.engine.current();
        return cur && cur->read([](const Capture& c) { return c.end > 2'000'000; });
    }));
    CHECK(r.engine.status().state == State::recording);
    r.engine.stop();
    REQUIRE(forge::testing::wait_until([&] { return r.done.load() == 1; }));
    CHECK(r.engine.status().stop_reason == "stopped");

    const Capture c = r.capture({.sample_hz = 100e6, .duration_s = 10, .max_edges = 5000}, 2);
    CHECK(c.truncated);
    CHECK(c.size() == 5000);
    CHECK(r.engine.status().stop_reason == "edge limit");
}

TEST_CASE("engine: words lost by the FPGA leave a gap in the capture") {
    std::vector<std::uint32_t> w;
    WireEncoder enc(w);
    enc.start(0);
    enc.change(100, 1);
    enc.change(200, 0);
    enc.gap(5000, 0b0011);  // lost in (200, 5000]
    enc.change(5100, 0b0010);
    enc.time(20000);
    Run r(std::make_unique<ScriptSource>(w));
    const Capture c = r.capture({.sample_hz = 1e6, .duration_s = 0.01});
    REQUIRE(c.gaps.size() == 1);
    CHECK(c.gaps[0].first == 200);
    CHECK(c.gaps[0].second == 5000);
    CHECK(c.levels_at(5000) == 0b0011);
    CHECK(c.levels_at(5100) == 0b0010);
    CHECK(c.end == 10000);

    // The gap survives a .lacap round trip.
    std::stringstream f;
    write_capture(c, f);
    const Capture back = read_capture(f);
    CHECK(back.gaps == c.gaps);
    CHECK(back.t == c.t);
    CHECK(back.lv == c.lv);
}

TEST_CASE("files: .lacap round trip, VCD and CSV") {
    Run r(sim("spi"));
    const Capture c = r.capture({.sample_hz = 50e6, .duration_s = 0.006, .trigger = "falling", .pre_trigger_ms = 0.01});
    std::stringstream f;
    write_capture(c, f, {{"note", "kept"}});
    forge::json extra;
    const Capture b = read_capture(f, &extra);
    CHECK(extra["note"] == "kept");
    CHECK(b.t == c.t);
    CHECK(b.lv == c.lv);
    CHECK(b.begin == c.begin);
    CHECK(b.end == c.end);
    CHECK(b.initial == c.initial);
    CHECK(b.trigger == c.trigger);
    CHECK(b.names == c.names);
    CHECK(b.tick_hz == c.tick_hz);
    CHECK(make_decoder("spi", normalize_settings("spi", {}))->decode(b).size() ==
          make_decoder("spi", normalize_settings("spi", {}))->decode(c).size());
    std::stringstream bad("LACAP1\n{\"tick_hz\": 0}\n");
    CHECK_THROWS(read_capture(bad));
    std::stringstream other("hello\n");
    CHECK_THROWS(read_capture(other));

    std::ostringstream vcd;
    write_vcd(c, vcd);
    const std::string v = vcd.str();
    CHECK(v.find("$timescale 1 ns $end") != std::string::npos);
    CHECK(v.find("$var wire 1 ! D0 $end") != std::string::npos);
    // every change has a time line: 20 ns per tick at 50 MHz
    CHECK(v.find(std::format("#{}\n", (c.t[0] - c.begin) * 20)) != std::string::npos);

    std::ostringstream csv;
    write_csv(c, csv);
    const std::string s = csv.str();
    CHECK(s.starts_with("time_s,D0,D1,D2,D3\n"));
    CHECK(static_cast<std::size_t>(std::count(s.begin(), s.end(), '\n')) == c.size() + 2);
}

TEST_CASE("files: the store keeps the newest captures") {
    forge::testing::TempDir dir;
    CaptureStore store(dir / "caps", 2);
    Capture c;
    c.tick_hz = 1e6;
    c.end = 1000;
    c.push(10, 1);
    std::vector<std::string> names;
    for (int i = 0; i < 3; ++i) {
        c.id = static_cast<std::uint64_t>(i + 1);
        c.started_ms = 1'700'000'000'000 + i * 1000;
        names.push_back(store.save(c));
        std::this_thread::sleep_for(20ms);  // distinct file times
    }
    const auto list = store.list();
    REQUIRE(list.size() == 2);
    CHECK(list[0]["file"] == names[2]);
    CHECK(list[0]["edges"] == 1);
    CHECK(store.load(names[1]).t == c.t);
    CHECK_THROWS_AS(store.path("../etc/passwd"), std::invalid_argument);
    CHECK_THROWS_AS(store.load(names[0]), std::invalid_argument);  // pruned
    CHECK(store.remove(names[1]));
    CHECK(store.list().size() == 1);
}
