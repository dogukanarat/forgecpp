// The fault-log example: faults are raised, cleared by time or by hand, and
// stay in the file across a restart.
#include <forge/forge.hpp>
#include <forge/testing.hpp>

// main.cpp ends with FORGE_MAIN(FaultLog); the test has its own main().
#undef FORGE_MAIN
#define FORGE_MAIN(AppClass)
#include "main.cpp"

using namespace std::chrono_literals;

namespace {

using Runner = forge::testing::AppRunner<FaultLog>;

std::vector<std::string> args(const forge::testing::TempDir& dir, double rate = 0.0) {
    return {"-D", "http.enabled=false",
            "-D", "faults.db=" + (dir / "faults.db").string(),
            "-D", "faults.rate=" + std::to_string(rate),
            "-D", "faults.clear_after_s=10"};
}

forge::json panel(Runner& r, const std::string& id) {
    return r->invoke([&] { return r->dashboard()->get(id); });
}

}  // namespace

TEST_CASE("fault-log: warnings clear themselves, errors wait for an operator") {
    forge::testing::TempDir dir;
    Runner r(args(dir), {.manual_time = true});
    const auto w = r->invoke([&] { return r->raise("eps", 101, faults::warning, "battery below 7.0 V"); });
    const auto e = r->invoke([&] { return r->raise("adcs", 202, faults::error, "reaction wheel 2 not spinning"); });
    CHECK(panel(r, "open") == 2);
    CHECK(panel(r, "open_errors") == 1);
    CHECK(panel(r, "last").get<std::string>().starts_with("#2 adcs error"));

    r.advance(11s);  // past clear_after_s: the warning has cleared, the error has not
    auto open = r->invoke([&] { return r->open_faults(); });
    REQUIRE(open.size() == 1);
    CHECK(open[0].id == e);
    CHECK(panel(r, "open") == 1);

    // The operator's button clears the error.
    r->invoke([&] { r->dashboard()->press("clear_errors"); });
    CHECK(panel(r, "open") == 0);
    CHECK_FALSE(r->invoke([&] { return r->clear(e); }));  // already cleared

    // Both are still in the log, with the time they cleared.
    const auto all = r->invoke([&] { return r->db().get_all<faults::Fault>(); });
    REQUIRE(all.size() == 2);
    CHECK(all[0].id == w);
    REQUIRE(all[0].cleared_ms.has_value());
    CHECK(*all[0].cleared_ms - all[0].time_ms >= 10000);
    CHECK(all[1].cleared_ms.has_value());
}

TEST_CASE("fault-log: the dummy source raises faults of the configured units") {
    forge::testing::TempDir dir;
    Runner r(args(dir, 0.5), {.manual_time = true});
    r.advance(100s);  // 100 checks
    const auto raised = panel(r, "raised").get<int>();
    CHECK(raised > 25);
    CHECK(raised < 75);
    const auto counted = r->invoke([&] { return r->db().count<faults::Fault>(); });
    CHECK(counted == raised);
    // Every fault points to a unit (the foreign key), and only to the four configured ones.
    const auto units = r->invoke([&] { return r->db().count<faults::Unit>(); });
    CHECK(units == 4);
}

TEST_CASE("fault-log: open faults are still open after a restart") {
    forge::testing::TempDir dir;
    {
        Runner r(args(dir), {.manual_time = true});
        r->invoke([&] { r->raise("radio", 303, faults::error, "synthesizer lost lock"); });
        r->invoke([&] { r->raise("radio", 301, faults::info, "frame with bad CRC"); });
    }
    Runner r(args(dir), {.manual_time = true});
    CHECK(panel(r, "open") == 2);
    CHECK(r->invoke([&] { return r->clear_all(); }) == 2);
    CHECK(panel(r, "open") == 0);
}

TEST_CASE("fault-log: the log keeps the newest faults") {
    forge::testing::TempDir dir;
    auto a = args(dir);
    a.insert(a.end(), {"-D", "faults.keep=10"});
    Runner r(a, {.manual_time = true});
    for (int i = 0; i < 25; ++i) r->invoke([&] { r->raise("eps", 102, faults::info, "solar array current low"); });
    const auto ids = r->invoke([&] { return r->db().select(&faults::Fault::id); });
    REQUIRE(ids.size() == 10);
    CHECK(ids.front() == 16);
    CHECK(ids.back() == 25);
}

TEST_CASE("fault-log: the console commands") {
    forge::testing::TempDir dir;
    Runner r(args(dir), {.manual_time = true});
    r->invoke([&] { r->raise("eps", 103, faults::error, "battery heater stuck on"); });
    r->invoke([&] { r->raise("radio", 301, faults::info, "frame with bad CRC"); });

    // Runs a line as a telnet session would, on the main loop; returns what it printed.
    auto run = [&](const std::string& line) {
        std::string out;
        forge::Console::Session session{.channel = "test", .output = [&](std::string_view s) { out += s; }};
        r->invoke([&] { CHECK(r->console()->execute(line, session)); });
        return out;
    };
    REQUIRE(r->console() != nullptr);
    const auto open = run("faults");
    CHECK(open.find("battery heater stuck on") != std::string::npos);
    CHECK(open.find("radio") != std::string::npos);

    const auto stats = run("stats");
    CHECK(stats.find("payload") != std::string::npos);  // a unit without faults is listed too
    CHECK(stats.find("open error") != std::string::npos);

    CHECK(run("clear 1").find("cleared") != std::string::npos);
    CHECK(run("clear 1").find("no open fault") != std::string::npos);
    CHECK(run("history eps").find("(cleared)") != std::string::npos);
    CHECK(run("history radio 5").find("battery") == std::string::npos);
    CHECK(run("history nosuch").find("no unit nosuch") != std::string::npos);
    CHECK(run("clear all").find("1 cleared") != std::string::npos);
}
