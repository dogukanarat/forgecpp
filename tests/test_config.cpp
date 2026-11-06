#include <forge/config.hpp>
#include <forge/testing.hpp>

#include <fstream>

using forge::Config;
using forge::ConfigError;

TEST_CASE("config: typed access with defaults") {
    const auto cfg = Config::parse(R"(
        name = "sensor"
        ratio = 0.5
        enabled = true

        [http]
        port = 8080
        hosts = ["a", "b"]

        [[servers]]
        host = "first"
    )");

    CHECK(cfg.get_or<std::string>("name", "") == "sensor");
    CHECK(cfg.get_or<double>("ratio", 0.0) == doctest::Approx(0.5));
    CHECK(cfg.get_or<bool>("enabled", false));
    CHECK(cfg.get_or<int>("http.port", 0) == 8080);
    CHECK(cfg.get_or<double>("http.port", 0.0) == doctest::Approx(8080.0));  // int -> float is fine
    CHECK(cfg.get_or<int>("http.missing", 42) == 42);
    CHECK(cfg.get_or<std::vector<std::string>>("http.hosts", {}) == std::vector<std::string>{"a", "b"});
    CHECK(cfg.require<std::string>("servers[0].host") == "first");
    CHECK(cfg.contains("http.port"));
    CHECK_FALSE(cfg.contains("nope"));
    CHECK_FALSE(cfg.get<int>("nope").has_value());
}

TEST_CASE("config: errors") {
    const auto cfg = Config::parse("port = \"eighty\"\nsmall = 300");
    CHECK_THROWS_AS(cfg.get<int>("port"), ConfigError);
    CHECK_THROWS_AS(cfg.require<int>("missing"), ConfigError);
    CHECK_THROWS_AS(cfg.get<std::int8_t>("small"), ConfigError);  // out of range
    CHECK_THROWS_AS(Config::parse("this is = not toml ="), ConfigError);
    CHECK_THROWS_AS(Config::load_file("/definitely/not/here.toml"), ConfigError);
}

TEST_CASE("config: overrides, set and merge") {
    auto cfg = Config::parse("[log]\nlevel = \"info\"");
    cfg.apply_override("log.level=debug");     // bare word -> string
    cfg.apply_override("http.port = 9000");    // integer, creates [http]
    cfg.apply_override("http.enabled=true");
    cfg.apply_override("peers=[\"x\", \"y\"]");
    cfg.set("device.id", 7);

    CHECK(cfg.get_or<std::string>("log.level", "") == "debug");
    CHECK(cfg.get_or<int>("http.port", 0) == 9000);
    CHECK(cfg.get_or<bool>("http.enabled", false));
    CHECK(cfg.get_or<std::vector<std::string>>("peers", {}).size() == 2);
    CHECK(cfg.get_or<int>("device.id", 0) == 7);
    CHECK_THROWS_AS(cfg.apply_override("no-equals-sign"), ConfigError);

    auto other = Config::parse("[log]\nfile = \"/tmp/x.log\"\n[http]\nport = 1");
    cfg.merge(other);
    CHECK(cfg.get_or<std::string>("log.level", "") == "debug");  // kept
    CHECK(cfg.get_or<std::string>("log.file", "") == "/tmp/x.log");
    CHECK(cfg.get_or<int>("http.port", 0) == 1);  // replaced
}

TEST_CASE("config: section, json and files") {
    forge::testing::TempDir dir;
    const auto path = dir / "app.toml";
    std::ofstream(path) << "[db]\nhost = \"localhost\"\nport = 5432\n";

    const auto cfg = Config::load_file(path);
    CHECK(cfg.source() == path);
    const auto db = cfg.section("db");
    CHECK(db.get_or<int>("port", 0) == 5432);
    CHECK(cfg.section("missing").empty());

    const auto j = cfg.to_json();
    CHECK(j["db"]["host"] == "localhost");
    CHECK(j["db"]["port"] == 5432);
    CHECK(cfg.get<nlohmann::json>("db.port").value() == 5432);
    CHECK(cfg.to_toml().find("localhost") != std::string::npos);
}
