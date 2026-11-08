#include <forge/log.hpp>
#include <forge/json.hpp>
#include <forge/log_rt.hpp>
#include <forge/testing.hpp>
#include <forge/trace.hpp>

#include <fstream>
#include <mutex>
#include <sstream>

namespace {

// Everything the terminal writer gets (the stderr replacement), plain and coloured.
struct Captured {
    std::mutex m;
    std::string plain, coloured;
    forge::log::TerminalWriter previous;
    Captured() {
        previous = forge::log::set_terminal_writer([this](std::string_view p, std::string_view c) {
            std::lock_guard lock(m);
            plain += p;
            coloured += c;
        });
    }
    ~Captured() { forge::log::set_terminal_writer(std::move(previous)); }
    std::string text() {
        forge::log::flush();
        std::lock_guard lock(m);
        return plain;
    }
};

forge::log::Options quiet(const std::string& name) {
    forge::log::Options o;
    o.name = name;
    o.flush_interval_s = 0;
    o.color = "never";
    return o;
}

}  // namespace

TEST_CASE("log: levels, named loggers, pattern, colours") {
    auto o = quiet("test");
    o.level = "debug";
    o.pattern = "{logger}|{level}|{message}";
    forge::log::init(o);
    Captured out;

    forge::log::info("hello {}", 1);
    forge::log::get("worker")->debug("from worker");
    forge::log::get("worker").warn("{:.1f} %", 99.25);
    forge::log::trace("hidden");
    FORGE_ERROR("macro {}", "too");
    const auto text = out.text();
    CHECK(text.find("test|INF|hello 1\n") != std::string::npos);
    CHECK(text.find("worker|DBG|from worker\n") != std::string::npos);
    CHECK(text.find("worker|WRN|99.2 %\n") != std::string::npos);
    CHECK(text.find("test|ERR|macro too\n") != std::string::npos);
    CHECK(text.find("hidden") == std::string::npos);
    {
        std::lock_guard lock(out.m);
        CHECK(out.coloured.find("\033[33m\033[1mWRN\033[0m") != std::string::npos);  // the level in its colour
    }

    forge::log::set_level("warn");
    CHECK(forge::log::level() == forge::log::Level::warn);
    CHECK(forge::log::get("worker").level() == forge::log::Level::warn);
    forge::log::set_logger_level("worker", "trace");
    CHECK(forge::log::get("worker").level() == forge::log::Level::trace);
    CHECK_THROWS_AS(forge::log::set_logger_level("nobody", "info"), std::invalid_argument);
    CHECK_THROWS_AS(forge::log::parse_level("loud"), std::invalid_argument);
    CHECK(forge::log::to_string(forge::log::parse_level("WARN")) == "warning");
    for (auto l : {forge::log::Level::trace, forge::log::Level::debug, forge::log::Level::info, forge::log::Level::warn,
                   forge::log::Level::error, forge::log::Level::critical}) {
        CHECK(forge::log::short_name(l).size() == 3);
    }
    CHECK(forge::log::short_name(forge::log::Level::critical) == "CRT");
    const auto levels = forge::log::logger_levels();
    CHECK(std::find(levels.begin(), levels.end(), std::pair<std::string, std::string>{"worker", "trace"}) !=
          levels.end());
    forge::log::set_level("info");

    o.pattern = "{nope}";
    CHECK_THROWS_AS(forge::log::init(o), std::invalid_argument);
}

TEST_CASE("log: history, listeners and the caller's trace context") {
    forge::log::init(quiet("hist"));
    Captured out;
    std::mutex m;
    std::vector<forge::log::Record> seen;
    auto sub = forge::log::add_listener([&](const forge::log::Record& r) {
        std::lock_guard lock(m);
        seen.push_back(r);
    });
    const auto ctx = forge::trace::new_trace();
    {
        forge::trace::ContextScope scope(ctx);
        forge::log::warn("inside a trace");
    }
    forge::log::info("outside");
    forge::log::flush();
    sub.reset();
    CHECK_FALSE(sub.active());

    std::lock_guard lock(m);
    REQUIRE(seen.size() >= 2);
    const auto& traced = seen[seen.size() - 2];
    CHECK(traced.message == "inside a trace");
    CHECK(traced.level == forge::log::Level::warn);
    CHECK(traced.logger == "hist");
    CHECK(traced.trace.trace_hex() == ctx.trace_hex());
    CHECK_FALSE(seen.back().trace.valid());
    const auto h = forge::log::history(1);
    REQUIRE(h.size() == 1);
    CHECK(h[0].message == "outside");
}

TEST_CASE("log: real-time macros format on the background thread") {
    forge::log::init(quiet("rt"));
    Captured out;
    const auto logger = forge::log::get("rt-thread");
    FORGE_RT_WARN(logger, "late by {} us on channel {}", 17, std::string_view("rx1"));
    CHECK(out.text().find("[rt-thread] late by 17 us on channel rx1") != std::string::npos);
}

TEST_CASE("log: JSON lines and the rotating file") {
    forge::testing::TempDir dir;
    auto o = quiet("filetest");
    o.console = false;
    o.file = (dir / "logs/app.log").string();
    o.max_size_mb = 1;
    o.max_files = 2;
    o.format = "json";
    forge::log::init(o);
    const std::string big(1000, 'x');
    for (int i = 0; i < 2500; ++i) forge::log::info("line {} {}", i, big);  // ~2.5 MB: two rotations
    forge::log::flush();
    o.file.clear();
    forge::log::init(o);  // closes the file
    CHECK(std::filesystem::exists(dir / "logs/app.log"));
    CHECK(std::filesystem::exists(dir / "logs/app.log.1"));
    CHECK(std::filesystem::exists(dir / "logs/app.log.2"));
    CHECK_FALSE(std::filesystem::exists(dir / "logs/app.log.3"));
    std::ifstream in(dir / "logs/app.log");
    std::string first;
    std::getline(in, first);
    const auto j = forge::json::parse(first);
    CHECK(j["level"] == "info");
    CHECK(j["logger"] == "filetest");
    CHECK(j["message"].get<std::string>().rfind("line ", 0) == 0);
}
