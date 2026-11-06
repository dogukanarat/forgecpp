#include <forge/log.hpp>
#include <forge/testing.hpp>

#include <spdlog/sinks/ostream_sink.h>

#include <sstream>

TEST_CASE("log: levels, named loggers and extra sinks") {
    forge::log::Options o;
    o.name = "test";
    o.console = false;
    o.level = "debug";
    o.flush_interval_s = 0;
    forge::log::init(o);

    std::ostringstream os;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(os);
    sink->set_pattern("%n|%l|%v");
    forge::log::add_sink(sink);

    forge::log::info("hello {}", 1);
    forge::log::get("worker")->debug("from worker");
    forge::log::trace("hidden");
    forge::log::flush();

    const auto out = os.str();
    CHECK(out.find("test|info|hello 1") != std::string::npos);
    CHECK(out.find("worker|debug|from worker") != std::string::npos);
    CHECK(out.find("hidden") == std::string::npos);

    forge::log::set_level("warn");
    CHECK(forge::log::level() == spdlog::level::warn);
    CHECK(forge::log::get("worker")->level() == spdlog::level::warn);
    CHECK_THROWS_AS(forge::log::parse_level("loud"), std::invalid_argument);

    forge::log::remove_sink(sink);
    forge::log::set_level("info");
}

TEST_CASE("log: file sink") {
    forge::testing::TempDir dir;
    forge::log::Options o;
    o.name = "filetest";
    o.console = false;
    o.file = (dir / "logs/app.log").string();
    o.flush_interval_s = 0;
    forge::log::init(o);
    forge::log::info("to file");
    forge::log::flush();
    CHECK(std::filesystem::file_size(o.file) > 0);

    o.file.clear();
    forge::log::init(o);  // release the file before TempDir is removed
}
